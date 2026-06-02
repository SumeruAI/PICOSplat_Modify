/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatComponent.h"

#include "Geometry/SplatConvexHull.h"
#include "Misc/AssertionMacros.h"
#include "Rendering/SplatSceneProxy.h"

#if WITH_EDITOR
#include "Engine.h"
#endif

using PICO::Splat::FSplatSceneProxy;

namespace
{
/**
 * Removes outlier points that are far from the cluster center.
 * Uses median + factor * MAD (Median Absolute Deviation) as threshold.
 */
TArray<FVector3f> RemoveOutlierPoints(
	TConstArrayView<FVector3f> Points, float DistanceFactor = 3.0f)
{
	if (Points.Num() < 8 || DistanceFactor <= 0.0f)
	{
		return TArray<FVector3f>(Points);
	}

	// Compute centroid.
	FVector3f Center = FVector3f::ZeroVector;
	for (const FVector3f& P : Points)
	{
		Center += P;
	}
	Center /= static_cast<float>(Points.Num());

	// Compute distances to centroid.
	TArray<float> Distances;
	Distances.SetNumUninitialized(Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		Distances[Index] = FVector3f::Distance(Points[Index], Center);
	}

	// Compute median distance.
	TArray<float> SortedDistances(Distances);
	SortedDistances.Sort();
	const int32 Mid = SortedDistances.Num() / 2;
	const float MedianDist = (SortedDistances.Num() & 1)
		? SortedDistances[Mid]
		: 0.5f * (SortedDistances[Mid - 1] + SortedDistances[Mid]);

	// Compute MAD (Median Absolute Deviation).
	TArray<float> Deviations;
	Deviations.SetNumUninitialized(Distances.Num());
	for (int32 Index = 0; Index < Distances.Num(); ++Index)
	{
		Deviations[Index] = FMath::Abs(Distances[Index] - MedianDist);
	}
	Deviations.Sort();
	const float MAD = (Deviations.Num() & 1)
		? Deviations[Mid]
		: 0.5f * (Deviations[Mid - 1] + Deviations[Mid]);

	// Threshold: points beyond this are outliers.
	const float Threshold = MedianDist + DistanceFactor * FMath::Max(MAD, 0.01f);

	TArray<FVector3f> Filtered;
	Filtered.Reserve(Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		if (Distances[Index] <= Threshold)
		{
			Filtered.Add(Points[Index]);
		}
	}

	// Safety: don't trim too aggressively (keep at least 50% of points).
	if (Filtered.Num() < Points.Num() / 2 || Filtered.Num() < 4)
	{
		return TArray<FVector3f>(Points);
	}

	return Filtered;
}
} // namespace

// Defined here to avoid needing scene proxy to be module public.
FPrimitiveSceneProxy* USplatComponent::CreateSceneProxy()
{
	// Note: Unreal expects a new here, and will handle deletion itself.
	return Asset ? new FSplatSceneProxy{*this} : nullptr;
}

void USplatComponent::SetSplatAsset(USplatAsset* InAsset)
{
	if (Asset == InAsset)
	{
		return;
	}

	Asset = InAsset;
	BodySetup = nullptr;

	UpdateBounds();
	MarkRenderStateDirty();
	RecreatePhysicsState();
}

void USplatComponent::RebuildCollision()
{
	if (!Asset)
	{
		return;
	}

	FString Error;
	const bool bRebuilt = Asset->RebuildCollisionFromStoredData(
		USplatAsset::MakeCollisionBuildSettingsSnapshot(), Error);
	if (!bRebuilt)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RebuildCollision failed for splat component %s: %s"),
			*GetReadableName(), *Error);
		return;
	}

	// Drop cached body setup so it is rebuilt from the asset's new hulls,
	// then refresh physics state and the editor wireframe proxy.
	BodySetup = nullptr;
	RecreatePhysicsState();
	UpdateBounds();
	MarkRenderStateDirty();
}

#if WITH_EDITOR
void USplatComponent::RebuildFromSource()
{
	if (!Asset)
	{
		return;
	}

	if (!Asset->HasSourceData() && !Asset->HasSourceFilePath())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RebuildFromSource: splat asset %s has no cached source import data or source PLY path. Re-import the PLY first."),
			*Asset->GetPathName());
		return;
	}

	FString Error;
	const bool bRebuilt = Asset->RebuildFromSource(
		USplatAsset::MakeCollisionBuildSettingsSnapshot(), Error);
	if (!bRebuilt)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RebuildFromSource failed for splat component %s: %s"),
			*GetReadableName(), *Error);
		return;
	}

	// USplatAsset::RebuildFromSource already iterated over dependent
	// components, but our own BodySetup cache also needs to drop.
	BodySetup = nullptr;
	RecreatePhysicsState();
	UpdateBounds();
	MarkRenderStateDirty();
}

void USplatComponent::StripCachedSourceData()
{
	if (Asset)
	{
		Asset->StripCachedSourceData();
	}
}
#endif

UBodySetup* USplatComponent::GetBodySetup()
{
	if (!Asset)
	{
		return nullptr;
	}
	else if (!BodySetup)
	{
		BodySetup = NewObject<UBodySetup>();

		FKAggregateGeom AggGeom;

		if (bForceSingleConvexHull)
		{
			// Compute a true single convex hull, filtering out distant outliers.
			TConstArrayView<FVector3f> AllPositions = Asset->GetPositions();
			TArray<FVector3f> FilteredPositions = RemoveOutlierPoints(AllPositions);
			TArray<FVector3f> HullVertices;
			TArray<uint32> HullIndices;
			if (FilteredPositions.Num() >= 4 &&
			    PICO::Splat::GenerateConvexHull(FilteredPositions, HullVertices, HullIndices))
			{
				FKConvexElem Convex;
				Convex.VertexData.SetNumUninitialized(HullVertices.Num());
				for (int32 Index = 0; Index < HullVertices.Num(); ++Index)
				{
					Convex.VertexData[Index] = FVector(HullVertices[Index]);
				}
				Convex.IndexData.SetNumUninitialized(HullIndices.Num());
				for (int32 Index = 0; Index < HullIndices.Num(); ++Index)
				{
					Convex.IndexData[Index] = static_cast<int32>(HullIndices[Index]);
				}
				Convex.UpdateElemBox();
				AggGeom.ConvexElems.Add(MoveTemp(Convex));
			}
		}
		else
		{
			for (const FSplatCollisionHull& Hull : Asset->GetCollisionHulls())
			{
				if (Hull.Vertices.Num() < 4)
				{
					continue;
				}

				FKConvexElem Convex;
				Convex.VertexData.AddUninitialized(Hull.Vertices.Num());
				for (int32 Index = 0; Index < Hull.Vertices.Num(); ++Index)
				{
					Convex.VertexData[Index] = FVector(Hull.Vertices[Index]);
				}
				Convex.IndexData.SetNumUninitialized(Hull.Indices.Num());
				for (int32 Index = 0; Index < Hull.Indices.Num(); ++Index)
				{
					Convex.IndexData[Index] = static_cast<int32>(Hull.Indices[Index]);
				}
				Convex.UpdateElemBox();
				AggGeom.ConvexElems.Add(MoveTemp(Convex));
			}

			if (AggGeom.ConvexElems.IsEmpty())
			{
				TConstArrayView<FVector3f> ConvexHullVertices =
					Asset->GetConvexHullVertices();
				TConstArrayView<uint32> ConvexHullIndices =
					Asset->GetConvexHullIndices();
				if (ConvexHullVertices.Num() >= 4 && ConvexHullIndices.Num() >= 3)
				{
					FKConvexElem Convex;
					Convex.VertexData.AddUninitialized(ConvexHullVertices.Num());
					for (int32 Index = 0; Index < ConvexHullVertices.Num(); ++Index)
					{
						Convex.VertexData[Index] = FVector(ConvexHullVertices[Index]);
					}
					Convex.IndexData.SetNumUninitialized(ConvexHullIndices.Num());
					for (int32 Index = 0; Index < ConvexHullIndices.Num(); ++Index)
					{
						Convex.IndexData[Index] = static_cast<int32>(ConvexHullIndices[Index]);
					}
					Convex.UpdateElemBox();
					AggGeom.ConvexElems.Add(MoveTemp(Convex));
				}
			}
		}

		BodySetup->AddCollisionFrom(AggGeom);
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->CreatePhysicsMeshes();
	}

	return BodySetup;
}

#if WITH_EDITOR
void USplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropName = PropertyChangedEvent.GetMemberPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(USplatComponent, bForceSingleConvexHull))
	{
		BodySetup = nullptr;
		RecreatePhysicsState();
		UpdateBounds();
		MarkRenderStateDirty();
	}
}

void USplatComponent::GetUsedMaterials(
	TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (bGetDebugMaterials)
	{
		check(GEngine);
		OutMaterials.Add(GEngine->GeomMaterial);
		OutMaterials.Add(GEngine->ShadedLevelColorationUnlitMaterial);
		OutMaterials.Add(GEngine->WireframeMaterial);
	}
}
#endif

FBoxSphereBounds
USplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (Asset)
	{
		return Asset->GetRenderBounds().TransformBy(LocalToWorld);
	}

	return FBoxSphereBounds(
		LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f);
}
