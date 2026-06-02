#include "Geometry/SplatConvexHull.h"

#include "CompGeom/ConvexHull3.h"
#include "Containers/Map.h"
#include "Logging.h"
#include "SplatConstants.h"

using PICO::Splat::MetersToCentimeters;

namespace PICO::Splat
{
namespace
{
void MaybeAddIndex(TMap<uint32, uint32>& IndexMap, uint32 Index)
{
	if (!IndexMap.Contains(Index))
	{
		IndexMap.Add(Index, IndexMap.Num());
	}
}

TMap<uint32, uint32>
RemapIndices(TConstArrayView<UE::Geometry::FIndex3i> Indices)
{
	TMap<uint32, uint32> IndexMap{};

	for (const auto& Index3 : Indices)
	{
		MaybeAddIndex(IndexMap, Index3.A);
		MaybeAddIndex(IndexMap, Index3.B);
		MaybeAddIndex(IndexMap, Index3.C);
	}

	return IndexMap;
}
} // namespace

bool GenerateConvexHull(
	TConstArrayView<FVector3f> Positions,
	TArray<FVector3f>& OutVertices,
	TArray<uint32>& OutIndices)
{
	UE::Geometry::TConvexHull3<float> ConvexHull{};
	bool bSuccess = ConvexHull.Solve<FVector3f>(Positions);
	if (!bSuccess)
	{
		PICO_LOGE("Failed to solve for convex hull.");
		return false;
	}

	TArray<UE::Geometry::FIndex3i> HullIndices = ConvexHull.MoveTriangles();
	TMap<uint32, uint32> IndexMap = RemapIndices(HullIndices);

	OutIndices.SetNumUninitialized(HullIndices.Num() * 3);
	for (int32 Index = 0; Index < HullIndices.Num(); ++Index)
	{
		OutIndices[Index * 3 + 0] = IndexMap[HullIndices[Index].A];
		OutIndices[Index * 3 + 1] = IndexMap[HullIndices[Index].B];
		OutIndices[Index * 3 + 2] = IndexMap[HullIndices[Index].C];
	}

	OutVertices.SetNumUninitialized(IndexMap.Num());
	for (const auto& Pair : IndexMap)
	{
		OutVertices[Pair.Value] = MetersToCentimeters * Positions[Pair.Key];
	}

	return true;
}
} // namespace PICO::Splat
