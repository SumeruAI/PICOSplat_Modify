#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SplatRuntimeBlueprintLibrary.generated.h"

class ASplatActor;
class USplatAsset;

UCLASS()
class PICOSPLATRUNTIME_API USplatRuntimeBlueprintLibrary
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "PICO Splat")
	static USplatAsset* LoadSplatAssetFromPLYFile(
		const FString& FilePath,
		UObject* Outer,
		bool& bSuccess,
		FString& ErrorMessage);

	UFUNCTION(
		BlueprintCallable,
		Category = "PICO Splat",
		meta = (WorldContext = "WorldContextObject"))
	static ASplatActor* SpawnSplatActorFromPLYFile(
		UObject* WorldContextObject,
		const FString& FilePath,
		const FTransform& Transform,
		bool& bSuccess,
		FString& ErrorMessage);
};
