#pragma once

#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "SplatAsset.h"

class PICOSPLATRUNTIME_API FSplatRuntimeLoader
{
public:
	static bool LoadFromPLYMemory(
		TConstArrayView<uint8> Bytes,
		FRuntimeSplatBuildData& OutData,
		FString& OutError);

	static bool LoadFromPLYFile(
		const FString& FilePath,
		FRuntimeSplatBuildData& OutData,
		FString& OutError);
};
