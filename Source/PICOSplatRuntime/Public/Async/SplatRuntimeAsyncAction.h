#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "SplatRuntimeAsyncAction.generated.h"

class ASplatActor;
class USplatAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FSplatAssetAsyncResult,
	USplatAsset*,
	Asset,
	const FString&,
	ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FSplatActorAsyncResult,
	ASplatActor*,
	Actor,
	const FString&,
	ErrorMessage);

UCLASS()
class PICOSPLATRUNTIME_API ULoadSplatAssetFromPLYFileAsyncAction
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSplatAssetAsyncResult Completed;

	UPROPERTY(BlueprintAssignable)
	FSplatAssetAsyncResult Failed;

	UFUNCTION(
		BlueprintCallable,
		Category = "PICO Splat",
		meta =
			(BlueprintInternalUseOnly = "true",
		     WorldContext = "WorldContextObject"))
	static ULoadSplatAssetFromPLYFileAsyncAction* LoadSplatAssetFromPLYFileAsync(
		UObject* WorldContextObject,
		const FString& FilePath,
		UObject* Outer);

	virtual void Activate() override;

private:
	void BroadcastCompleted(USplatAsset* Asset, const FString& ErrorMessage);
	void BroadcastFailed(const FString& ErrorMessage);

	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<UObject> Outer;
	FString FilePath;
	bool bUseTransientOuter = false;
	bool bActivated = false;
};

UCLASS()
class PICOSPLATRUNTIME_API USpawnSplatActorFromPLYFileAsyncAction
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSplatActorAsyncResult Completed;

	UPROPERTY(BlueprintAssignable)
	FSplatActorAsyncResult Failed;

	UFUNCTION(
		BlueprintCallable,
		Category = "PICO Splat",
		meta =
			(BlueprintInternalUseOnly = "true",
		     WorldContext = "WorldContextObject"))
	static USpawnSplatActorFromPLYFileAsyncAction* SpawnSplatActorFromPLYFileAsync(
		UObject* WorldContextObject,
		const FString& FilePath,
		const FTransform& Transform);

	virtual void Activate() override;

private:
	void BroadcastCompleted(ASplatActor* Actor, const FString& ErrorMessage);
	void BroadcastFailed(const FString& ErrorMessage);

	TWeakObjectPtr<UObject> WorldContextObject;
	FString FilePath;
	FTransform Transform = FTransform::Identity;
	bool bActivated = false;
};