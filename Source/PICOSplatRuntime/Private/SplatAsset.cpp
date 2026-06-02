/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatAsset.h"

#include "Geometry/SplatConvexHull.h"
#include "SplatConstants.h"
#include "SplatSettings.h"

#include <algorithm>

#include "Containers/Set.h"
#include "Math/UnrealMathUtility.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Serialization/CustomVersion.h"

#if WITH_EDITOR
#include "Import/SplatRuntimeLoader.h"
#include "SplatComponent.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#endif

using PICO::Splat::FPackedCovMat;
using PICO::Splat::FPackedPos64;
using PICO::Splat::MetersToCentimeters;
using PICO::Splat::TSplatStaticBuffer;

namespace
{
constexpr int32 MinCollisionHullPoints = 4;
constexpr float MaxPackedCovarianceValue = 30000.0f;

FVector3f CalculateBoundsExtentMeters(TConstArrayView<FVector3f> Positions)
{
	if (Positions.IsEmpty())
	{
		return FVector3f::ZeroVector;
	}

	FVector3f BoundsMin(std::numeric_limits<float>::max());
	FVector3f BoundsMax(std::numeric_limits<float>::lowest());
	for (const FVector3f& Position : Positions)
	{
		BoundsMin = BoundsMin.ComponentMin(Position);
		BoundsMax = BoundsMax.ComponentMax(Position);
	}

	return BoundsMax - BoundsMin;
}

float CalculateScalePercentileMeters(
	TConstArrayView<FVector3f> ScalesMeters,
	float Percentile)
{
	if (ScalesMeters.IsEmpty())
	{
		return 0.0f;
	}

	TArray<float> Radii;
	Radii.Reserve(ScalesMeters.Num());
	for (const FVector3f& Scale : ScalesMeters)
	{
		Radii.Add(Scale.GetMax());
	}

	const int32 PercentileIndex = FMath::Clamp(
		FMath::RoundToInt((Radii.Num() - 1) * Percentile),
		0,
		Radii.Num() - 1);
	std::nth_element(
		Radii.GetData(),
		Radii.GetData() + PercentileIndex,
		Radii.GetData() + Radii.Num());
	return Radii[PercentileIndex];
}

/**
 * Robust per-axis outlier rejection using median + K * MAD (Median Absolute
 * Deviation). All four parallel arrays are filtered in lockstep so indices
 * stay aligned.
 *
 * Why MAD instead of percentiles: raw 3DGS PLY captures often contain a
 * sizable fraction (1-10%) of stray reconstruction-noise splats spread
 * across a much larger volume than the real scene. The fraction is large
 * enough that simple percentile trimming (e.g. 0.1% / 99.9%) cannot find
 * the real BBox edge. MAD locates the dense central population
 * (real scene) and uses it as a reference scale, ignoring how many
 * outliers there are or how far away they are.
 *
 * For data clustered in a region of half-width R, MAD ≈ R/2 (uniform) or
 * ≈ 0.6745 R (Gaussian σ). KeepRadius = K * MAD gives a fence around the
 * median. K = 5 keeps essentially all real splats while dropping noise
 * sitting well outside the dense cluster.
 *
 * Cost: 6 nth_element passes (O(N) each) plus one O(N) compaction.
 */
bool DropPositionOutliers(
	TArray<FVector3f>& Positions,
	TArray<FQuat4f>& Rotations,
	TArray<FVector3f>& Scales,
	TArray<FColor>& Colors,
	TArray<FVector4f>& SphericalHarmonics,
	float MADMultiplier,
	float MaxTrimFraction,
	int32 MinPointsForRejection)
{
	const int32 N = Positions.Num();
	if (N < MinPointsForRejection || MADMultiplier <= 0.f)
	{
		return false;
	}

	const int32 MedianIdx = N / 2;

	FVector3f Median;
	FVector3f KeepRadius;
	{
		TArray<float> Buf;
		Buf.SetNumUninitialized(N);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			// Per-axis median.
			for (int32 Index = 0; Index < N; ++Index)
			{
				Buf[Index] = Positions[Index][Axis];
			}
			std::nth_element(
				Buf.GetData(),
				Buf.GetData() + MedianIdx,
				Buf.GetData() + N);
			Median[Axis] = Buf[MedianIdx];

			// Per-axis MAD: median of |x - median|.
			for (int32 Index = 0; Index < N; ++Index)
			{
				Buf[Index] = FMath::Abs(Positions[Index][Axis] - Median[Axis]);
			}
			std::nth_element(
				Buf.GetData(),
				Buf.GetData() + MedianIdx,
				Buf.GetData() + N);
			float MAD = Buf[MedianIdx];
			// Floor MAD so axes with extreme symmetry don't collapse to zero.
			MAD = FMath::Max(MAD, KINDA_SMALL_NUMBER);
			KeepRadius[Axis] = MADMultiplier * MAD;
		}
	}

	const FVector3f LoCutoff = Median - KeepRadius;
	const FVector3f HiCutoff = Median + KeepRadius;

	TArray<uint8> KeepMask;
	KeepMask.SetNumUninitialized(N);
	int32 NumKept = 0;
	for (int32 Read = 0; Read < N; ++Read)
	{
		const FVector3f& P = Positions[Read];
		KeepMask[Read] =
			P.X >= LoCutoff.X && P.X <= HiCutoff.X &&
			P.Y >= LoCutoff.Y && P.Y <= HiCutoff.Y &&
			P.Z >= LoCutoff.Z && P.Z <= HiCutoff.Z;
		if (KeepMask[Read])
		{
			++NumKept;
		}
	}

	const int32 Dropped = N - NumKept;
	const float RemovedFraction =
		N > 0 ? static_cast<float>(Dropped) / static_cast<float>(N) : 0.0f;
	if (Dropped <= 0)
	{
		return false;
	}

	PICO_LOGL(
		"DropPositionOutliers (MAD x %.1f): candidate drop %d / %d splats. "
		"Median=(%.2f, %.2f, %.2f) m, KeepRadius=(%.2f, %.2f, %.2f) m, "
		"clip=[(%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f)] m.",
		MADMultiplier,
		Dropped,
		N,
		Median.X,
		Median.Y,
		Median.Z,
		KeepRadius.X,
		KeepRadius.Y,
		KeepRadius.Z,
		LoCutoff.X,
		LoCutoff.Y,
		LoCutoff.Z,
		HiCutoff.X,
		HiCutoff.Y,
		HiCutoff.Z);

	if (NumKept < N / 2 || NumKept < 4 || RemovedFraction > MaxTrimFraction)
	{
		PICO_LOGW(
			"Skipped render outlier rejection because it would drop %d / %d splats (%.2f%%), above the allowed trim fraction %.2f%%.",
			Dropped,
			N,
			100.0f * RemovedFraction,
			100.0f * MaxTrimFraction);
		return false;
	}

	int32 Write = 0;
	constexpr int32 SHCoeffCount =
		FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients;
	const bool bHasSphericalHarmonics =
		SphericalHarmonics.Num() == N * SHCoeffCount;
	for (int32 Read = 0; Read < N; ++Read)
	{
		if (!KeepMask[Read])
		{
			continue;
		}
		if (Write != Read)
		{
			Positions[Write] = Positions[Read];
			Rotations[Write] = Rotations[Read];
			Scales[Write] = Scales[Read];
			Colors[Write] = Colors[Read];
			if (bHasSphericalHarmonics)
			{
				for (int32 CoeffIndex = 0; CoeffIndex < SHCoeffCount; ++CoeffIndex)
				{
					SphericalHarmonics[Write * SHCoeffCount + CoeffIndex] =
						SphericalHarmonics[Read * SHCoeffCount + CoeffIndex];
				}
			}
		}
		++Write;
	}

	Positions.SetNum(Write, EAllowShrinking::No);
	Rotations.SetNum(Write, EAllowShrinking::No);
	Scales.SetNum(Write, EAllowShrinking::No);
	Colors.SetNum(Write, EAllowShrinking::No);
	if (bHasSphericalHarmonics)
	{
		SphericalHarmonics.SetNum(Write * SHCoeffCount, EAllowShrinking::No);
	}
	return true;
}

struct FPICOSplatAssetVersion
{
	enum Type
	{
		BeforeCustomVersionWasAdded = 0,
		AddedCollisionExtents,
		AddedSourceImportData,
		AddedSphericalHarmonics,
		AddedCovarianceScale,
		AddedSourceFilePath,
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1,
	};

	static const FGuid GUID;
};

const FGuid FPICOSplatAssetVersion::GUID(
	0x9C220C6E,
	0x5E3B4383,
	0xA872F5F6,
	0xC17C8D4A);

FCustomVersionRegistration GPICOSplatAssetVersionRegistration(
	FPICOSplatAssetVersion::GUID,
	FPICOSplatAssetVersion::LatestVersion,
	TEXT("PICOSplatAssetVersion"));

struct FCollisionVoxelComponent
{
	TArray<FIntVector> Voxels;
	int32 Weight = 0;
};

struct FFilteredCollisionPoints
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> ExtentsMeters;
	bool HasExtents = false;
};

FVector3f GetCollisionExtentMeters(
	TConstArrayView<FVector3f> CollisionExtentsMeters,
	int32 Index)
{
	return CollisionExtentsMeters.IsValidIndex(Index)
		       ? CollisionExtentsMeters[Index]
		       : FVector3f::ZeroVector;
}

FIntVector QuantizeCollisionVoxel(
	const FVector3f& PositionMeters,
	const FSplatCollisionBuildSettings& CollisionSettings)
{
	const float CollisionVoxelSizeMeters = CollisionSettings.CollisionVoxelSizeMeters;
	return FIntVector(
		FMath::FloorToInt(PositionMeters.X / CollisionVoxelSizeMeters),
		FMath::FloorToInt(PositionMeters.Y / CollisionVoxelSizeMeters),
		FMath::FloorToInt(PositionMeters.Z / CollisionVoxelSizeMeters));
}

template <typename TVisitor>
void ForEachOccupiedCollisionVoxel(
	const FVector3f& PositionMeters,
	const FVector3f& CollisionExtentMeters,
	const FSplatCollisionBuildSettings& CollisionSettings,
	TVisitor&& Visitor)
{
	const float CollisionVoxelSizeMeters = CollisionSettings.CollisionVoxelSizeMeters;
	const float CollisionRadiusScale = CollisionSettings.CollisionRadiusScale;
	const int32 MaxCollisionExpansionVoxels =
		CollisionSettings.MaxCollisionExpansionVoxels;
	const FVector3f ExpandedExtentMeters =
		CollisionRadiusScale * CollisionExtentMeters.ComponentMax(FVector3f::ZeroVector);

	const FIntVector CenterVoxel =
		QuantizeCollisionVoxel(PositionMeters, CollisionSettings);
	const FIntVector ExtentVoxels(
		FMath::Min(
			MaxCollisionExpansionVoxels,
			FMath::CeilToInt(ExpandedExtentMeters.X / CollisionVoxelSizeMeters)),
		FMath::Min(
			MaxCollisionExpansionVoxels,
			FMath::CeilToInt(ExpandedExtentMeters.Y / CollisionVoxelSizeMeters)),
		FMath::Min(
			MaxCollisionExpansionVoxels,
			FMath::CeilToInt(ExpandedExtentMeters.Z / CollisionVoxelSizeMeters)));

	for (int32 X = CenterVoxel.X - ExtentVoxels.X;
	     X <= CenterVoxel.X + ExtentVoxels.X;
	     ++X)
	{
		for (int32 Y = CenterVoxel.Y - ExtentVoxels.Y;
		     Y <= CenterVoxel.Y + ExtentVoxels.Y;
		     ++Y)
		{
			for (int32 Z = CenterVoxel.Z - ExtentVoxels.Z;
			     Z <= CenterVoxel.Z + ExtentVoxels.Z;
			     ++Z)
			{
				Visitor(FIntVector(X, Y, Z));
			}
		}
	}
}

TMap<FIntVector, int32> BuildCollisionVoxelCounts(
	TConstArrayView<FVector3f> Positions,
	TConstArrayView<FVector3f> CollisionExtentsMeters,
	const FSplatCollisionBuildSettings& CollisionSettings)
{
	TMap<FIntVector, int32> VoxelCounts;
	VoxelCounts.Reserve(Positions.Num());

	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		ForEachOccupiedCollisionVoxel(
			Positions[Index],
			GetCollisionExtentMeters(CollisionExtentsMeters, Index),
			CollisionSettings,
			[&VoxelCounts](const FIntVector& Voxel)
			{
				++VoxelCounts.FindOrAdd(Voxel);
			});
	}

	return VoxelCounts;
}

FFilteredCollisionPoints FilterCollisionHullPoints(
	TConstArrayView<FVector3f> Positions,
	TConstArrayView<FVector3f> CollisionExtentsMeters,
	const FSplatCollisionBuildSettings& CollisionSettings)
{
	FFilteredCollisionPoints Filtered;
	Filtered.HasExtents = CollisionExtentsMeters.Num() == Positions.Num();

	const int32 MinPointsPerCollisionVoxel =
		CollisionSettings.MinPointsPerCollisionVoxel;
	if (Positions.Num() < MinPointsPerCollisionVoxel)
	{
		Filtered.Positions = TArray<FVector3f>(Positions);
		if (Filtered.HasExtents)
		{
			Filtered.ExtentsMeters = TArray<FVector3f>(CollisionExtentsMeters);
		}
		return Filtered;
	}

	TMap<FIntVector, int32> VoxelCounts =
		BuildCollisionVoxelCounts(
			Positions, CollisionExtentsMeters, CollisionSettings);

	TSet<FIntVector> DenseVoxels;
	for (const auto& Pair : VoxelCounts)
	{
		if (Pair.Value >= MinPointsPerCollisionVoxel)
		{
			DenseVoxels.Add(Pair.Key);
		}
	}

	if (DenseVoxels.IsEmpty())
	{
		Filtered.Positions = TArray<FVector3f>(Positions);
		if (Filtered.HasExtents)
		{
			Filtered.ExtentsMeters = TArray<FVector3f>(CollisionExtentsMeters);
		}
		return Filtered;
	}

	Filtered.Positions.Reserve(Positions.Num());
	if (Filtered.HasExtents)
	{
		Filtered.ExtentsMeters.Reserve(CollisionExtentsMeters.Num());
	}

	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		bool bTouchesDenseVoxel = false;
		ForEachOccupiedCollisionVoxel(
			Positions[Index],
			GetCollisionExtentMeters(CollisionExtentsMeters, Index),
			CollisionSettings,
			[&DenseVoxels, &bTouchesDenseVoxel](const FIntVector& Voxel)
			{
				bTouchesDenseVoxel = bTouchesDenseVoxel || DenseVoxels.Contains(Voxel);
			});

		if (bTouchesDenseVoxel)
		{
			Filtered.Positions.Add(Positions[Index]);
			if (Filtered.HasExtents)
			{
				Filtered.ExtentsMeters.Add(CollisionExtentsMeters[Index]);
			}
		}
	}

	if (Filtered.Positions.Num() < MinCollisionHullPoints)
	{
		Filtered.Positions = TArray<FVector3f>(Positions);
		if (Filtered.HasExtents)
		{
			Filtered.ExtentsMeters = TArray<FVector3f>(CollisionExtentsMeters);
		}
		return Filtered;
	}

	PICO_LOGL(
		"Filtered collision hull points from %d to %d using dense expanded voxels.",
		Positions.Num(),
		Filtered.Positions.Num());
	return Filtered;
}

TArray<FCollisionVoxelComponent>
FindCollisionVoxelComponents(
	TConstArrayView<FVector3f> Positions,
	TConstArrayView<FVector3f> CollisionExtentsMeters,
	const FSplatCollisionBuildSettings& CollisionSettings)
{
	const int32 MinPointsPerCollisionVoxel =
		CollisionSettings.MinPointsPerCollisionVoxel;
	TMap<FIntVector, int32> VoxelCounts =
		BuildCollisionVoxelCounts(
			Positions, CollisionExtentsMeters, CollisionSettings);

	TSet<FIntVector> DenseVoxels;
	for (const auto& Pair : VoxelCounts)
	{
		if (Pair.Value >= MinPointsPerCollisionVoxel)
		{
			DenseVoxels.Add(Pair.Key);
		}
	}

	TArray<FCollisionVoxelComponent> Components;
	if (DenseVoxels.IsEmpty())
	{
		return Components;
	}

	TSet<FIntVector> Visited;
	Visited.Reserve(DenseVoxels.Num());

	for (const FIntVector& StartVoxel : DenseVoxels)
	{
		if (Visited.Contains(StartVoxel))
		{
			continue;
		}

		FCollisionVoxelComponent Component;
		TArray<FIntVector> Queue{StartVoxel};
		int32 QueueIndex = 0;
		Visited.Add(StartVoxel);

		while (QueueIndex < Queue.Num())
		{
			const FIntVector Current = Queue[QueueIndex++];
			Component.Voxels.Add(Current);
			Component.Weight += VoxelCounts.FindChecked(Current);

			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						if (DX == 0 && DY == 0 && DZ == 0)
						{
							continue;
						}

						const FIntVector Neighbor =
							Current + FIntVector(DX, DY, DZ);
						if (!DenseVoxels.Contains(Neighbor) ||
						    Visited.Contains(Neighbor))
						{
							continue;
						}

						Visited.Add(Neighbor);
						Queue.Add(Neighbor);
					}
				}
			}
		}

		Components.Add(MoveTemp(Component));
	}

	Components.Sort([](
		const FCollisionVoxelComponent& Left,
		const FCollisionVoxelComponent& Right)
	{
		return Left.Weight > Right.Weight;
	});

	return Components;
}

void BuildCombinedCollisionMesh(
	const TArray<FSplatCollisionHull>& CollisionHulls,
	TArray<FVector3f>& OutVertices,
	TArray<uint32>& OutIndices)
{
	OutVertices.Reset();
	OutIndices.Reset();

	int32 TotalVertices = 0;
	int32 TotalIndices = 0;
	for (const FSplatCollisionHull& Hull : CollisionHulls)
	{
		TotalVertices += Hull.Vertices.Num();
		TotalIndices += Hull.Indices.Num();
	}

	OutVertices.Reserve(TotalVertices);
	OutIndices.Reserve(TotalIndices);

	uint32 VertexOffset = 0;
	for (const FSplatCollisionHull& Hull : CollisionHulls)
	{
		OutVertices.Append(Hull.Vertices);
		for (uint32 Index : Hull.Indices)
		{
			OutIndices.Add(VertexOffset + Index);
		}
		VertexOffset += Hull.Vertices.Num();
	}
}

float ComputeMedian(TArray<float> Values)
{
	if (Values.IsEmpty())
	{
		return 0.0f;
	}

	Values.Sort();
	const int32 MiddleIndex = Values.Num() / 2;
	if ((Values.Num() & 1) == 0)
	{
		return 0.5f * (Values[MiddleIndex - 1] + Values[MiddleIndex]);
	}

	return Values[MiddleIndex];
}

TArray<FVector3f> TrimFarCollisionPoints(
	TConstArrayView<FVector3f> Points,
	const FSplatCollisionBuildSettings& CollisionSettings)
{
	const float DistanceFactor = CollisionSettings.CollisionOutlierDistanceFactor;
	const int32 MinPointsForTrim =
		CollisionSettings.MinPointsForCollisionOutlierRejection;
	if (DistanceFactor <= 0.0f || Points.Num() < MinPointsForTrim)
	{
		return TArray<FVector3f>(Points);
	}

	FVector3f Center = FVector3f::ZeroVector;
	for (const FVector3f& Point : Points)
	{
		Center += Point;
	}
	Center /= static_cast<float>(Points.Num());

	TArray<float> Distances;
	Distances.Reserve(Points.Num());
	for (const FVector3f& Point : Points)
	{
		Distances.Add(FVector3f::Distance(Point, Center));
	}

	const float MedianDistance = ComputeMedian(Distances);
	TArray<float> AbsoluteDeviations;
	AbsoluteDeviations.Reserve(Distances.Num());
	for (float Distance : Distances)
	{
		AbsoluteDeviations.Add(FMath::Abs(Distance - MedianDistance));
	}

	const float MAD = ComputeMedian(AbsoluteDeviations);
	const float DistanceFloor = CollisionSettings.CollisionVoxelSizeMeters;
	const float DistanceThreshold =
		MedianDistance + DistanceFactor * FMath::Max(MAD, 0.5f * DistanceFloor);

	TArray<FVector3f> TrimmedPoints;
	TrimmedPoints.Reserve(Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		if (Distances[Index] <= DistanceThreshold)
		{
			TrimmedPoints.Add(Points[Index]);
		}
	}

	const int32 NumRemoved = Points.Num() - TrimmedPoints.Num();
	const float RemovedFraction =
		Points.Num() > 0 ? static_cast<float>(NumRemoved) / Points.Num() : 0.0f;
	if (TrimmedPoints.Num() < MinCollisionHullPoints ||
	    RemovedFraction > CollisionSettings.CollisionOutlierMaxTrimFraction)
	{
		return TArray<FVector3f>(Points);
	}

	if (NumRemoved > 0)
	{
		PICO_LOGL(
			"Trimmed %d distant collision points out of %d before convex hull generation.",
			NumRemoved,
			Points.Num());
	}

	return TrimmedPoints;
}

bool GenerateCollisionHulls(
	TConstArrayView<FVector3f> Positions,
	TConstArrayView<FVector3f> CollisionExtentsMeters,
	const FSplatCollisionBuildSettings& CollisionSettings,
	TArray<FSplatCollisionHull>& OutCollisionHulls)
{
	OutCollisionHulls.Reset();

	// Single-hull mode: skip voxel clustering entirely.
	if (CollisionSettings.bForceSingleConvexHull)
	{
		const FFilteredCollisionPoints FilteredPoints =
			FilterCollisionHullPoints(
				Positions, CollisionExtentsMeters, CollisionSettings);
		if (FilteredPoints.Positions.Num() < MinCollisionHullPoints)
		{
			return false;
		}
		const TArray<FVector3f>& TrimmedPoints =
			TrimFarCollisionPoints(FilteredPoints.Positions, CollisionSettings);
		const TArray<FVector3f>& HullPoints =
			TrimmedPoints.Num() >= MinCollisionHullPoints
			? TrimmedPoints
			: FilteredPoints.Positions;
		FSplatCollisionHull SingleHull;
		if (!PICO::Splat::GenerateConvexHull(
				HullPoints, SingleHull.Vertices, SingleHull.Indices))
		{
			return false;
		}
		OutCollisionHulls.Add(MoveTemp(SingleHull));
		PICO_LOGL(
			"Generated single convex hull from %d filtered splat points.",
			FilteredPoints.Positions.Num());
		return true;
	}

	const FFilteredCollisionPoints FilteredPoints =
		FilterCollisionHullPoints(
			Positions, CollisionExtentsMeters, CollisionSettings);
	const TArray<FCollisionVoxelComponent> Components =
		FindCollisionVoxelComponents(
			FilteredPoints.Positions,
			FilteredPoints.ExtentsMeters,
			CollisionSettings);
	const int32 MinPointsPerCollisionComponent =
		CollisionSettings.MinPointsPerCollisionComponent;

	if (!Components.IsEmpty())
	{
		TMap<FIntVector, int32> VoxelToComponent;
		const int32 NumComponents =
			FMath::Min(Components.Num(), CollisionSettings.MaxCollisionHulls);
		for (int32 ComponentIndex = 0; ComponentIndex < NumComponents;
		     ++ComponentIndex)
		{
			for (const FIntVector& Voxel : Components[ComponentIndex].Voxels)
			{
				VoxelToComponent.Add(Voxel, ComponentIndex);
			}
		}

		TArray<TArray<FVector3f>> ComponentPoints;
		ComponentPoints.SetNum(NumComponents);
		for (int32 PointIndex = 0; PointIndex < FilteredPoints.Positions.Num();
		     ++PointIndex)
		{
			int32 OwningComponentIndex = INDEX_NONE;
			ForEachOccupiedCollisionVoxel(
				FilteredPoints.Positions[PointIndex],
				GetCollisionExtentMeters(FilteredPoints.ExtentsMeters, PointIndex),
				CollisionSettings,
				[&VoxelToComponent, &OwningComponentIndex](const FIntVector& Voxel)
				{
					if (OwningComponentIndex == INDEX_NONE)
					{
						if (const int32* FoundComponentIndex =
								VoxelToComponent.Find(Voxel))
						{
							OwningComponentIndex = *FoundComponentIndex;
						}
					}
				});

			if (OwningComponentIndex != INDEX_NONE)
			{
				ComponentPoints[OwningComponentIndex].Add(
					FilteredPoints.Positions[PointIndex]);
			}
		}

		for (TArray<FVector3f>& Points : ComponentPoints)
		{
			if (Points.Num() < MinPointsPerCollisionComponent ||
			    Points.Num() < MinCollisionHullPoints)
			{
				continue;
			}

			TArray<FVector3f> TrimmedPoints =
				TrimFarCollisionPoints(Points, CollisionSettings);
			const TArray<FVector3f>& HullPoints =
				TrimmedPoints.Num() >= MinCollisionHullPoints ? TrimmedPoints : Points;

			FSplatCollisionHull Hull;
			if (PICO::Splat::GenerateConvexHull(
					HullPoints,
					Hull.Vertices,
					Hull.Indices))
			{
				OutCollisionHulls.Add(MoveTemp(Hull));
			}
		}
	}

	if (!OutCollisionHulls.IsEmpty())
	{
		PICO_LOGL(
			"Generated %d collision hulls from %d filtered splat points.",
			OutCollisionHulls.Num(),
			FilteredPoints.Positions.Num());
		return true;
	}

	if (FilteredPoints.Positions.Num() < MinCollisionHullPoints)
	{
		return false;
	}

	FSplatCollisionHull FallbackHull;
	if (!PICO::Splat::GenerateConvexHull(
			FilteredPoints.Positions, FallbackHull.Vertices, FallbackHull.Indices))
	{
		return false;
	}

	OutCollisionHulls.Add(MoveTemp(FallbackHull));
	PICO_LOGL(
		"Fell back to a single collision hull built from %d filtered splat points.",
		FilteredPoints.Positions.Num());
	return true;
}
} // namespace

FSplatCollisionBuildSettings USplatAsset::MakeCollisionBuildSettingsSnapshot()
{
	FSplatCollisionBuildSettings Settings;
	Settings.CollisionVoxelSizeMeters =
		USplatSettings::GetCollisionVoxelSizeMeters();
	Settings.MinPointsPerCollisionVoxel =
		USplatSettings::GetMinPointsPerCollisionVoxel();
	Settings.MinPointsPerCollisionComponent =
		USplatSettings::GetMinPointsPerCollisionComponent();
	Settings.MaxCollisionHulls = USplatSettings::GetMaxCollisionHulls();
	Settings.CollisionRadiusScale = USplatSettings::GetCollisionRadiusScale();
	Settings.MaxCollisionExpansionVoxels =
		USplatSettings::GetMaxCollisionExpansionVoxels();
	Settings.CollisionOutlierDistanceFactor =
		USplatSettings::GetCollisionOutlierDistanceFactor();
	Settings.CollisionOutlierMaxTrimFraction =
		USplatSettings::GetCollisionOutlierMaxTrimFraction();
	Settings.MinPointsForCollisionOutlierRejection =
		USplatSettings::GetMinPointsForCollisionOutlierRejection();
	Settings.bEnableRenderOutlierRejection =
		USplatSettings::IsRenderOutlierRejectionEnabled();
	Settings.RenderOutlierMADMultiplier =
		USplatSettings::GetRenderOutlierMADMultiplier();
	Settings.RenderOutlierMaxTrimFraction =
		USplatSettings::GetRenderOutlierMaxTrimFraction();
	Settings.MinPointsForRenderOutlierRejection =
		USplatSettings::GetMinPointsForRenderOutlierRejection();
	return Settings;
}

bool USplatAsset::BuildPreparedRuntimeData(
	FRuntimeSplatBuildData&& InData,
	const FSplatCollisionBuildSettings& InCollisionSettings,
	FRuntimeSplatPreparedData& OutPreparedData,
	FString& OutError)
{
	const int32 Num = InData.PositionsMeters.Num();
	if (Num <= 0)
	{
		OutError = TEXT("Splat data is empty.");
		return false;
	}

	const int32 ExpectedSHCount =
		Num * FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients;
	if (InData.Rotations.Num() != Num || InData.ScalesMeters.Num() != Num ||
	    InData.Colors.Num() != Num ||
		(!InData.SphericalHarmonics.IsEmpty() &&
		 InData.SphericalHarmonics.Num() != ExpectedSHCount))
	{
		OutError =
			TEXT("Splat data arrays must all have the same number of elements.");
		return false;
	}

	OutPreparedData.BuildData = MoveTemp(InData);

	// Drop position outliers (raw 3DGS PLY noise) before any downstream
	// processing. Uses median + K * MAD so it is robust to large noise
	// fractions (commonly 1-10% in raw 3DGS captures). All four parallel
	// arrays are filtered in lockstep.
	if (InCollisionSettings.bEnableRenderOutlierRejection)
	{
		DropPositionOutliers(
			OutPreparedData.BuildData.PositionsMeters,
			OutPreparedData.BuildData.Rotations,
			OutPreparedData.BuildData.ScalesMeters,
			OutPreparedData.BuildData.Colors,
			OutPreparedData.BuildData.SphericalHarmonics,
			InCollisionSettings.RenderOutlierMADMultiplier,
			InCollisionSettings.RenderOutlierMaxTrimFraction,
			InCollisionSettings.MinPointsForRenderOutlierRejection);
	}

	if (!GenerateCollisionHulls(
			OutPreparedData.BuildData.PositionsMeters,
			OutPreparedData.BuildData.ScalesMeters,
			InCollisionSettings,
			OutPreparedData.CollisionHulls))
	{
		OutError = TEXT("Failed to generate collision hulls for the splat asset.");
		return false;
	}

	BuildCombinedCollisionMesh(
		OutPreparedData.CollisionHulls,
		OutPreparedData.ConvexHullVertices,
		OutPreparedData.ConvexHullIndices);

	FVector3f BoundsMin(std::numeric_limits<float>::max());
	FVector3f BoundsMax(std::numeric_limits<float>::lowest());
	for (const FVector3f& Position : OutPreparedData.BuildData.PositionsMeters)
	{
		BoundsMin = BoundsMin.ComponentMin(Position);
		BoundsMax = BoundsMax.ComponentMax(Position);
	}
	const FVector3f BoundsExtent = BoundsMax - BoundsMin;
	const float MedianScale = CalculateScalePercentileMeters(
		OutPreparedData.BuildData.ScalesMeters,
		0.5f);
	const float P95Scale = CalculateScalePercentileMeters(
		OutPreparedData.BuildData.ScalesMeters,
		0.95f);
	const float P99Scale = CalculateScalePercentileMeters(
		OutPreparedData.BuildData.ScalesMeters,
		0.99f);

	PICO_LOGL(
		"Prepared runtime splat data: splats %d -> %d, bounds min=(%.2f, %.2f, %.2f) m, max=(%.2f, %.2f, %.2f) m, extent=(%.2f, %.2f, %.2f) m, scale median/p95/p99=(%.4f, %.4f, %.4f) m, collision hulls=%d, collision vertices=%d, collision indices=%d.",
		Num,
		OutPreparedData.BuildData.PositionsMeters.Num(),
		BoundsMin.X,
		BoundsMin.Y,
		BoundsMin.Z,
		BoundsMax.X,
		BoundsMax.Y,
		BoundsMax.Z,
		BoundsExtent.X,
		BoundsExtent.Y,
		BoundsExtent.Z,
		MedianScale,
		P95Scale,
		P99Scale,
		OutPreparedData.CollisionHulls.Num(),
		OutPreparedData.ConvexHullVertices.Num(),
		OutPreparedData.ConvexHullIndices.Num());
	return true;
}

bool USplatAsset::RebuildCollisionFromStoredData(
	const FSplatCollisionBuildSettings& InCollisionSettings,
	FString& OutError)
{
	if (PositionsFullPrecision.IsEmpty())
	{
		OutError = TEXT("Splat asset has no stored positions to rebuild collision from.");
		return false;
	}

	TArray<FSplatCollisionHull> NewHulls;
	if (!GenerateCollisionHulls(
			PositionsFullPrecision,
			CollisionExtentsMeters,
			InCollisionSettings,
			NewHulls))
	{
		OutError = TEXT("Failed to regenerate collision hulls.");
		return false;
	}

	TArray<FVector3f> NewCombinedVertices;
	TArray<uint32> NewCombinedIndices;
	BuildCombinedCollisionMesh(NewHulls, NewCombinedVertices, NewCombinedIndices);

	CollisionHulls = MoveTemp(NewHulls);
	ConvexHullVertices = MoveTemp(NewCombinedVertices);
	ConvexHullIndices = MoveTemp(NewCombinedIndices);

	PICO_LOGL(
		"Rebuilt collision: hulls=%d, vertices=%d, indices=%d.",
		CollisionHulls.Num(),
		ConvexHullVertices.Num(),
		ConvexHullIndices.Num());
	return true;
}

#if WITH_EDITOR
void USplatAsset::SetSourceImportData(const FRuntimeSplatBuildData& InSourceData)
{
	Modify();
	SourcePositionsMeters = InSourceData.PositionsMeters;
	SourceRotations = InSourceData.Rotations;
	SourceScalesMeters = InSourceData.ScalesMeters;
	SourceColors = InSourceData.Colors;
	SourceSphericalHarmonics = InSourceData.SphericalHarmonics;
	UpdateAssetDiagnostics();
}

void USplatAsset::ClearSourceImportData()
{
	if (SourcePositionsMeters.IsEmpty()
		&& SourceRotations.IsEmpty()
		&& SourceScalesMeters.IsEmpty()
		&& SourceColors.IsEmpty()
		&& SourceSphericalHarmonics.IsEmpty())
	{
		return;
	}

	Modify();
	SourcePositionsMeters.Empty();
	SourceRotations.Empty();
	SourceScalesMeters.Empty();
	SourceColors.Empty();
	SourceSphericalHarmonics.Empty();
	UpdateAssetDiagnostics();
	MarkPackageDirty();
}

void USplatAsset::SetSourceFilePath(const FString& InSourceFilePath)
{
	Modify();
	SourceFilePath = InSourceFilePath;
#if WITH_EDITORONLY_DATA
	if (!AssetImportData)
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
	AssetImportData->Update(SourceFilePath);
#endif
	UpdateAssetDiagnostics();
	MarkPackageDirty();
}

void USplatAsset::StripCachedSourceData()
{
	ClearSourceImportData();
}

bool USplatAsset::RebuildFromSource(
	const FSplatCollisionBuildSettings& InCollisionSettings,
	FString& OutError)
{
	if (SourceFilePath.IsEmpty() && !HasSourceData())
	{
		OutError = TEXT("Splat asset has no cached source import data or stored source PLY path; re-import the PLY to enable rebuilds.");
		return false;
	}

	FRuntimeSplatBuildData SourceCopy;
	if (!SourceFilePath.IsEmpty())
	{
		if (!FPaths::FileExists(SourceFilePath))
		{
			OutError = FString::Printf(
				TEXT("Stored source PLY path does not exist: %s"),
				*SourceFilePath);
			return false;
		}

		if (!FSplatRuntimeLoader::LoadFromPLYFile(SourceFilePath, SourceCopy, OutError))
		{
			return false;
		}
	}
	else
	{
		SourceCopy.PositionsMeters = SourcePositionsMeters;
		SourceCopy.Rotations = SourceRotations;
		SourceCopy.ScalesMeters = SourceScalesMeters;
		SourceCopy.Colors = SourceColors;
		SourceCopy.SphericalHarmonics = SourceSphericalHarmonics;
	}

	FRuntimeSplatPreparedData PreparedData;
	if (!BuildPreparedRuntimeData(
			MoveTemp(SourceCopy), InCollisionSettings, PreparedData, OutError))
	{
		return false;
	}

	// Detach SceneProxies from any USplatComponent referencing this asset
	// before tearing down RHI resources, so render thread releases the
	// last references to the SRVs we are about to free.
	for (TObjectIterator<USplatComponent> It; It; ++It)
	{
		USplatComponent* Component = *It;
		if (IsValid(Component) && Component->GetAsset() == this)
		{
			Component->MarkRenderStateDirty();
		}
	}

	// Tear down GPU buffers and wait for the render thread to release them.
	if (Positions)
	{
		BeginReleaseResource(&*Positions);
	}
	if (CovariancesCM)
	{
		BeginReleaseResource(&*CovariancesCM);
	}
	if (Colors)
	{
		BeginReleaseResource(&*Colors);
	}
	if (SphericalHarmonics)
	{
		BeginReleaseResource(&*SphericalHarmonics);
	}
	ReleaseResourcesFence.BeginFence();
	ReleaseResourcesFence.Wait();

	Positions.reset();
	CovariancesCM.reset();
	Colors.reset();
	SphericalHarmonics.reset();

	// Re-run the standard initialization path.
	bRuntimeInitialized = false;
	Modify();
	if (!InitializeFromPreparedRuntimeData(MoveTemp(PreparedData), OutError))
	{
		return false;
	}

	// Refresh dependent components again now that GPU buffers are valid.
	for (TObjectIterator<USplatComponent> It; It; ++It)
	{
		USplatComponent* Component = *It;
		if (IsValid(Component) && Component->GetAsset() == this)
		{
			Component->MarkRenderStateDirty();
			Component->RecreatePhysicsState();
			Component->UpdateBounds();
		}
	}

	PICO_LOGL(
		"Rebuilt splat asset %s from source: splats=%u, hulls=%d.",
		*GetPathName(),
		NumSplats,
		CollisionHulls.Num());
	UpdateAssetDiagnostics();
	return true;
}
#endif // WITH_EDITOR

void USplatAsset::UpdateAssetDiagnostics()
{
#if WITH_EDITORONLY_DATA
	bHasCachedSourceData = HasSourceData();
	SourceSplatCount = bHasCachedSourceData ? SourcePositionsMeters.Num() : 0;
	CurrentSplatCount = static_cast<int32>(NumSplats);
	CollisionHullCount = CollisionHulls.Num();

	const FSplatCollisionBuildSettings Settings = MakeCollisionBuildSettingsSnapshot();
	ImportSettingsSummary = FString::Printf(
		TEXT("Preset=%s, RenderOutlier=%s, MAD=%.2f, MaxTrim=%.1f%%, Hulls=%d, Voxel=%.2fm"),
		*UEnum::GetValueAsString(USplatSettings::GetQualityPreset()),
		Settings.bEnableRenderOutlierRejection ? TEXT("On") : TEXT("Off"),
		Settings.RenderOutlierMADMultiplier,
		100.0f * Settings.RenderOutlierMaxTrimFraction,
		Settings.MaxCollisionHulls,
		Settings.CollisionVoxelSizeMeters);

	const FVector3f BoundsExtentMeters = CalculateBoundsExtentMeters(PositionsFullPrecision);
	RenderPrecisionSummary = FString::Printf(
		TEXT("Bounds extent: %.2f x %.2f x %.2f m\nUNorm64 position step: %.6f / %.6f / %.6f cm\nCovariance scale: %.4f cm^2"),
		BoundsExtentMeters.X,
		BoundsExtentMeters.Y,
		BoundsExtentMeters.Z,
		PosScaleCM.X,
		PosScaleCM.Y,
		PosScaleCM.Z,
		CovarianceScaleCM2);

	if (bHasCachedSourceData)
	{
		const int32 DroppedSplats = FMath::Max(0, SourceSplatCount - CurrentSplatCount);
		AssetStatus = FString::Printf(
			TEXT("Rebuild From Source available. Current import kept %d / %d splats (dropped %d)."),
			CurrentSplatCount,
			SourceSplatCount,
			DroppedSplats);
	}
	else
	{
		AssetStatus = SourceFilePath.IsEmpty()
			? TEXT("No cached source data or source path. Re-import the PLY to enable Rebuild From Source.")
			: FString::Printf(
				TEXT("Rebuild From Source will reload: %s"),
				*SourceFilePath);
	}
#endif
}

void USplatAsset::BeginDestroy()
{
	Super::BeginDestroy();

	// Default Asset will have None for buffers.
	if (Positions)
	{
		BeginReleaseResource(&*Positions);
	}
	if (CovariancesCM)
	{
		BeginReleaseResource(&*CovariancesCM);
	}
	if (Colors)
	{
		BeginReleaseResource(&*Colors);
	}
	if (SphericalHarmonics)
	{
		BeginReleaseResource(&*SphericalHarmonics);
	}

	ReleaseResourcesFence.BeginFence();
}

void USplatAsset::BeginInit()
{
	check(Positions);
	check(CovariancesCM);
	check(Colors);
	check(SphericalHarmonics);

	FName Name = FName(GetPathName());

	Positions->SetOwnerName(Name);
	BeginInitResource(&*Positions);
	CovariancesCM->SetOwnerName(Name);
	BeginInitResource(&*CovariancesCM);
	Colors->SetOwnerName(Name);
	BeginInitResource(&*Colors);
	SphericalHarmonics->SetOwnerName(Name);
	BeginInitResource(&*SphericalHarmonics);

#if !WITH_EDITOR
	FlushRenderingCommands();
#endif
}

bool USplatAsset::IsReadyForFinishDestroy()
{
	return ReleaseResourcesFence.IsFenceComplete();
}

bool USplatAsset::InitializeFromRuntimeData(
	const FRuntimeSplatBuildData& InData, FString& OutError)
{
	FRuntimeSplatPreparedData PreparedData;
	if (!BuildPreparedRuntimeData(
			FRuntimeSplatBuildData(InData),
			MakeCollisionBuildSettingsSnapshot(),
			PreparedData,
			OutError))
	{
		return false;
	}

	return InitializeFromPreparedRuntimeData(MoveTemp(PreparedData), OutError);
}

bool USplatAsset::InitializeFromPreparedRuntimeData(
	FRuntimeSplatPreparedData&& InPreparedData,
	FString& OutError)
{
	if (bRuntimeInitialized)
	{
		OutError = TEXT("Splat asset is already initialized.");
		return false;
	}

	const FRuntimeSplatBuildData& BuildData = InPreparedData.BuildData;
	const int32 Num = BuildData.PositionsMeters.Num();
	if (Num <= 0)
	{
		OutError = TEXT("Splat data is empty.");
		return false;
	}

	const int32 ExpectedSHCount =
		Num * FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients;
	if (BuildData.Rotations.Num() != Num || BuildData.ScalesMeters.Num() != Num ||
	    BuildData.Colors.Num() != Num ||
		(!BuildData.SphericalHarmonics.IsEmpty() &&
		 BuildData.SphericalHarmonics.Num() != ExpectedSHCount))
	{
		OutError =
			TEXT("Splat data arrays must all have the same number of elements.");
		return false;
	}

	SetNumSplats(static_cast<uint32>(Num));
	SetPositionsMeters(TArray<FVector3f>(BuildData.PositionsMeters));
	SetCollisionExtentsMeters(TArray<FVector3f>(BuildData.ScalesMeters));
	SetCovariancesQuatScaleMeters(BuildData.Rotations, BuildData.ScalesMeters);
	SetColorsLinear(TArray<FColor>(BuildData.Colors));
	TArray<FVector4f> SHData;
	if (BuildData.SphericalHarmonics.Num() == ExpectedSHCount)
	{
		SHData = BuildData.SphericalHarmonics;
	}
	else
	{
		SHData.SetNumZeroed(ExpectedSHCount);
	}
	SetSphericalHarmonics(MoveTemp(SHData));
	CollisionHulls = MoveTemp(InPreparedData.CollisionHulls);
	ConvexHullVertices = MoveTemp(InPreparedData.ConvexHullVertices);
	ConvexHullIndices = MoveTemp(InPreparedData.ConvexHullIndices);

	BeginInit();
	bRuntimeInitialized = true;
	UpdateAssetDiagnostics();
	return true;
}

void USplatAsset::PostLoad()
{
	Super::PostLoad();

	if (CollisionExtentsMeters.Num() != PositionsFullPrecision.Num())
	{
		CollisionExtentsMeters.Reset();
	}

	if (GenerateCollisionHulls(
			PositionsFullPrecision,
			CollisionExtentsMeters,
			MakeCollisionBuildSettingsSnapshot(),
			CollisionHulls))
	{
		BuildCombinedCollisionMesh(
			CollisionHulls, ConvexHullVertices, ConvexHullIndices);
	}
	else if (ConvexHullVertices.Num() >= 4 && ConvexHullIndices.Num() >= 3)
	{
		FSplatCollisionHull LegacyHull;
		LegacyHull.Vertices = ConvexHullVertices;
		LegacyHull.Indices = ConvexHullIndices;
		CollisionHulls.Reset();
		CollisionHulls.Add(MoveTemp(LegacyHull));
		PICO_LOGW(
			"Fell back to legacy single-hull collision data for %s.",
			*GetPathName());
	}
	else
	{
		CollisionHulls.Reset();
		ConvexHullVertices.Reset();
		ConvexHullIndices.Reset();
		PICO_LOGW("No collision hull data could be generated for %s.", *GetPathName());
	}
	SetPositionsMetersInternal(PositionsFullPrecision);
	if (!SphericalHarmonics)
	{
		TArray<FVector4f> EmptySphericalHarmonics;
		EmptySphericalHarmonics.SetNumZeroed(
			static_cast<int32>(NumSplats) *
			FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients);
		SetSphericalHarmonics(MoveTemp(EmptySphericalHarmonics));
	}

	// If we are in the Editor, we cannot erase the full-precision positions else
	// we will save empty data in Serialize().
#if !WITH_EDITOR
	if (USplatSettings::IsSortingOnGPU())
	{
		PositionsFullPrecision.Empty();
	}
#endif

	BeginInit();
	bRuntimeInitialized = true;
	UpdateAssetDiagnostics();
}

void USplatAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FPICOSplatAssetVersion::GUID);
	const int32 AssetVersion = Ar.CustomVer(FPICOSplatAssetVersion::GUID);

	Ar << NumSplats;

	// We have to support the null case for `UObject::DeclareCustomVersions`,
	// which serializes the default (empty) object. If not, our checks in
	// TSplatStaticBuffer<T>::operator<< will trip.
	if (NumSplats > 0)
	{
		Ar << PositionsFullPrecision;
		if (AssetVersion >= FPICOSplatAssetVersion::AddedCollisionExtents)
		{
			Ar << CollisionExtentsMeters;
		}
		Ar << CovariancesCM << Colors;
		if (AssetVersion >= FPICOSplatAssetVersion::AddedCovarianceScale)
		{
			Ar << CovarianceScaleCM2;
		}
		else if (Ar.IsLoading())
		{
			CovarianceScaleCM2 = 1.0f;
		}
		if (AssetVersion >= FPICOSplatAssetVersion::AddedSphericalHarmonics)
		{
			Ar << SphericalHarmonics;
		}
		Ar << ConvexHullVertices << ConvexHullIndices;

		// Editor-only: original imported splat data, kept so the asset can
		// be rebuilt without re-importing the source PLY. Strip from cooked
		// packages by writing/reading empty arrays in that case.
		if (AssetVersion >= FPICOSplatAssetVersion::AddedSourceImportData)
		{
#if WITH_EDITORONLY_DATA
			const bool bStripSource = Ar.IsFilterEditorOnly();
			if (bStripSource)
			{
				if (Ar.IsSaving())
				{
					TArray<FVector3f> EmptyVec;
					TArray<FQuat4f> EmptyQuat;
					TArray<FColor> EmptyColor;
					TArray<FVector4f> EmptySH;
					Ar << EmptyVec << EmptyQuat << EmptyVec << EmptyColor;
					if (AssetVersion >= FPICOSplatAssetVersion::AddedSphericalHarmonics)
					{
						Ar << EmptySH;
					}
				}
				else
				{
					TArray<FVector3f> Discard;
					TArray<FQuat4f> DiscardQ;
					TArray<FColor> DiscardC;
					TArray<FVector4f> DiscardSH;
					Ar << Discard << DiscardQ << Discard << DiscardC;
					if (AssetVersion >= FPICOSplatAssetVersion::AddedSphericalHarmonics)
					{
						Ar << DiscardSH;
					}
				}
			}
			else
			{
				Ar << SourcePositionsMeters
				   << SourceRotations
				   << SourceScalesMeters
				   << SourceColors;
				if (AssetVersion >= FPICOSplatAssetVersion::AddedSphericalHarmonics)
				{
					Ar << SourceSphericalHarmonics;
				}
			}
#else
			// Cooked runtime: source arrays were serialized as empty, but
			// still read them to advance the archive correctly.
			TArray<FVector3f> Discard;
			TArray<FQuat4f> DiscardQ;
			TArray<FColor> DiscardC;
			TArray<FVector4f> DiscardSH;
			Ar << Discard << DiscardQ << Discard << DiscardC;
			if (AssetVersion >= FPICOSplatAssetVersion::AddedSphericalHarmonics)
			{
				Ar << DiscardSH;
			}
#endif
		}

		if (AssetVersion >= FPICOSplatAssetVersion::AddedSourceFilePath)
		{
#if WITH_EDITORONLY_DATA
			FString EmptySourcePath;
			if (Ar.IsFilterEditorOnly())
			{
				Ar << EmptySourcePath;
			}
			else
			{
				Ar << SourceFilePath;
			}
#else
			FString DiscardSourcePath;
			Ar << DiscardSourcePath;
#endif
		}
	}
}

void USplatAsset::SetCovariancesQuatScaleMeters(
	const TArray<FQuat4f>& Rotations, const TArray<FVector3f>& ScalesMeters)
{
	check(Rotations.Num() == NumSplats);
	check(ScalesMeters.Num() == NumSplats);

	TStaticMeshVertexData<FPackedCovMat> Data;
	Data.ResizeBuffer(NumSplats);

	TArray<FMatrix44f> CovarianceMatrices;
	CovarianceMatrices.SetNumUninitialized(Data.Num());
	float MaxAbsCovarianceElement = 0.0f;
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		FMatrix44f R = FRotationMatrix44f::Make(Rotations[Index]);
		FMatrix44f S =
			FScaleMatrix44f::Make(MetersToCentimeters * ScalesMeters[Index]);

		// Σ = R * S * S * R^-1.
		// Note: R^-1 = R^T.
		FMatrix44f Sigma = R.GetTransposed() * S * S * R;
		CovarianceMatrices[Index] = Sigma;
		MaxAbsCovarianceElement = FMath::Max(
			MaxAbsCovarianceElement,
			FMath::Max3(
				FMath::Abs(Sigma.M[0][0]),
				FMath::Abs(Sigma.M[0][1]),
				FMath::Abs(Sigma.M[0][2])));
		MaxAbsCovarianceElement = FMath::Max(
			MaxAbsCovarianceElement,
			FMath::Max3(
				FMath::Abs(Sigma.M[1][1]),
				FMath::Abs(Sigma.M[1][2]),
				FMath::Abs(Sigma.M[2][2])));
	}

	CovarianceScaleCM2 = FMath::Max(
		1.0f,
		MaxAbsCovarianceElement / MaxPackedCovarianceValue);
	const float InvCovarianceScaleCM2 = 1.0f / CovarianceScaleCM2;
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		FMatrix44f ScaledSigma = CovarianceMatrices[Index];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Column = 0; Column < 3; ++Column)
			{
				ScaledSigma.M[Row][Column] *= InvCovarianceScaleCM2;
			}
		}
		reinterpret_cast<FPackedCovMat*>(Data.GetDataPointer())[Index] =
			FPackedCovMat(ScaledSigma);
	}

	if (CovarianceScaleCM2 > 1.0f)
	{
		PICO_LOGL(
			"Splat covariance auto-scale enabled for %s: max |cov| %.2f cm^2, packed scale %.4f.",
			*GetPathName(),
			MaxAbsCovarianceElement,
			CovarianceScaleCM2);
	}

	CovariancesCM = TSplatStaticBuffer(std::move(Data));
}

void USplatAsset::SetPositionsMeters(TArray<FVector3f>&& PositionsMeters)
{
	// Do not condition this on sorting implementation. This is executed within
	// the editor at import-time, and must be present to be saved to disk and ran
	// with whatever sorting method is in use at runtime.
	PositionsFullPrecision = std::move(PositionsMeters);

	SetPositionsMetersInternal(PositionsFullPrecision);
}

void USplatAsset::SetPositionsMetersInternal(
	const TArray<FVector3f>& PositionsMeters)
{
	check(PositionsMeters.Num() == NumSplats);

	/**
	 * Find per-axis BBox for 21/21/22-bit UNorm position quantization.
	 *
	 * Outlier splats are expected to have been dropped up-front by
	 * `DropPositionOutliers` in `BuildPreparedRuntimeData`. This makes the
	 * strict min/max safe to use here without losing precision to noise
	 * reconstruction points. A defensive Clamp(0,1) on the normalized
	 * position is kept so that any caller bypassing the runtime build path
	 * still produces valid UNorm values rather than wrapping.
	 */
	FVector3f PosMaxM(std::numeric_limits<float>::lowest());
	FVector3f PosMinM(std::numeric_limits<float>::max());
	for (const FVector3f& PosM : PositionsMeters)
	{
		PosMaxM = PosMaxM.ComponentMax(PosM);
		PosMinM = PosMinM.ComponentMin(PosM);
	}
	check(PosMaxM.GetMin() > std::numeric_limits<float>::lowest());
	check(PosMinM.GetMax() < std::numeric_limits<float>::max());

	// Guard against a degenerate axis (all values equal).
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (PosMaxM[Axis] <= PosMinM[Axis])
		{
			PosMaxM[Axis] = PosMinM[Axis] + KINDA_SMALL_NUMBER;
		}
	}

	PosScaleCM = MetersToCentimeters * (PosMaxM - PosMinM) / FPackedPos64::MAX;
	PosMaxCM = MetersToCentimeters * PosMaxM;
	PosMinCM = MetersToCentimeters * PosMinM;

	PICO_LOGL(
		"Splat position packing for %s: bounds extent=(%.2f, %.2f, %.2f) m, UNorm64 step=(%.6f, %.6f, %.6f) cm.",
		*GetPathName(),
		(PosMaxM - PosMinM).X,
		(PosMaxM - PosMinM).Y,
		(PosMaxM - PosMinM).Z,
		PosScaleCM.X,
		PosScaleCM.Y,
		PosScaleCM.Z);

	TStaticMeshVertexData<FPackedPos64> Data{/*InNeedsCPUAccess=*/false};
	Data.ResizeBuffer(NumSplats);
	const FVector3f Range = PosMaxM - PosMinM;
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		FVector3f Normalized = (PositionsMeters[Index] - PosMinM) / Range;
		Normalized.X = FMath::Clamp(Normalized.X, 0.f, 1.f);
		Normalized.Y = FMath::Clamp(Normalized.Y, 0.f, 1.f);
		Normalized.Z = FMath::Clamp(Normalized.Z, 0.f, 1.f);
		reinterpret_cast<FPackedPos64*>(Data.GetDataPointer())[Index] =
			Normalized;
	}
	Positions = TSplatStaticBuffer(std::move(Data));
}