#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformAtomics.h"
#include "Templates/SharedPointer.h"

class ASplatActor;
class USplatAsset;

DECLARE_DELEGATE_ThreeParams(
	FOnSplatAssetLoadCompleted,
	USplatAsset*,
	bool,
	const FString&);

DECLARE_DELEGATE_ThreeParams(
	FOnSplatActorSpawnCompleted,
	ASplatActor*,
	bool,
	const FString&);

/**
 * Lightweight handle for cancelling an in-flight async splat load. Cancellation
 * is cooperative and checked between the major load phases (file load,
 * prepare, asset construction). It does not interrupt blocking I/O, but it
 * prevents the completion delegate from running and avoids unnecessary work
 * after engine shutdown was requested.
 */
class PICOSPLATRUNTIME_API FSplatRuntimeAsyncHandle
{
public:
	bool IsCancelled() const
	{
		return FPlatformAtomics::AtomicRead(&CancelledFlag) != 0;
	}

	void Cancel()
	{
		FPlatformAtomics::InterlockedExchange(&CancelledFlag, 1);
	}

private:
	mutable volatile int32 CancelledFlag = 0;
};

class PICOSPLATRUNTIME_API FSplatRuntimeAsync
{
public:
	static TSharedRef<FSplatRuntimeAsyncHandle>
	LoadSplatAssetFromPLYFileAsync(
		const FString& FilePath,
		UObject* Outer,
		FOnSplatAssetLoadCompleted Completion);

	static TSharedRef<FSplatRuntimeAsyncHandle>
	SpawnSplatActorFromPLYFileAsync(
		UObject* WorldContextObject,
		const FString& FilePath,
		const FTransform& Transform,
		FOnSplatActorSpawnCompleted Completion);
};