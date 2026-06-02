#pragma once

#include "Containers/Array.h"
#include "Math/Vector.h"

namespace PICO::Splat
{
bool GenerateConvexHull(
	TConstArrayView<FVector3f> Positions,
	TArray<FVector3f>& OutVertices,
	TArray<uint32>& OutIndices);
}
