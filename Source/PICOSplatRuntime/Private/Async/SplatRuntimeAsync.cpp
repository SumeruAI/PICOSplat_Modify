#include "Async/SplatRuntimeAsync.h"

#include "Async/Async.h"
#include "Async/AsyncWork.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Import/SplatRuntimeLoader.h"
#include "Misc/CoreDelegates.h"
#include "SplatActor.h"
#include "SplatAsset.h"
#include "SplatComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
bool IsCancellationRequested(const TSharedRef<FSplatRuntimeAsyncHandle>& Handle)
{
	return Handle->IsCancelled() || IsEngineExitRequested();
}

struct FSplatPreparedDataLoadResult
{
	FRuntimeSplatPreparedData PreparedData;
	bool bSuccess = false;
	bool bCancelled = false;
	FString ErrorMessage;
};

USplatAsset* CreateRuntimeSplatAssetFromPreparedData(
	UObject* Outer,
	FRuntimeSplatPreparedData&& PreparedData,
	FString& OutError)
{
	if (!Outer)
	{
		Outer = GetTransientPackage();
	}

	USplatAsset* Asset = NewObject<USplatAsset>(Outer);
	if (!Asset)
	{
		OutError = TEXT("Failed to allocate a splat asset.");
		return nullptr;
	}

	if (!Asset->InitializeFromPreparedRuntimeData(
			MoveTemp(PreparedData), OutError))
	{
		return nullptr;
	}

	return Asset;
}

class FSplatPreparedDataLoadTask final : public FNonAbandonableTask
{
public:
	FSplatPreparedDataLoadTask(
		FString InFilePath,
		const FSplatCollisionBuildSettings& InCollisionSettings,
		TSharedRef<FSplatRuntimeAsyncHandle> InHandle,
		TUniqueFunction<void(FSplatPreparedDataLoadResult&&)>&& InCompletion)
		: FilePath(MoveTemp(InFilePath))
		, CollisionSettings(InCollisionSettings)
		, Handle(MoveTemp(InHandle))
		, Completion(MoveTemp(InCompletion))
	{
	}

	void DoWork()
	{
		FSplatPreparedDataLoadResult Result;

		if (IsCancellationRequested(Handle))
		{
			Result.bCancelled = true;
			DispatchCompletion(MoveTemp(Result));
			return;
		}

		FRuntimeSplatBuildData BuildData;
		if (!FSplatRuntimeLoader::LoadFromPLYFile(
				FilePath, BuildData, Result.ErrorMessage))
		{
			DispatchCompletion(MoveTemp(Result));
			return;
		}

		if (IsCancellationRequested(Handle))
		{
			Result.bCancelled = true;
			DispatchCompletion(MoveTemp(Result));
			return;
		}

		Result.bSuccess = USplatAsset::BuildPreparedRuntimeData(
			MoveTemp(BuildData),
			CollisionSettings,
			Result.PreparedData,
			Result.ErrorMessage);

		DispatchCompletion(MoveTemp(Result));
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(
			FSplatPreparedDataLoadTask,
			STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	friend class FAutoDeleteAsyncTask<FSplatPreparedDataLoadTask>;

	void DispatchCompletion(FSplatPreparedDataLoadResult&& Result)
	{
		// On engine shutdown the game thread task may never run, but we
		// must still post the completion so the calling site sees the
		// final state if the editor is still alive.
		AsyncTask(
			ENamedThreads::GameThread,
			[Handle = Handle, Completion = MoveTemp(Completion),
			 Result = MoveTemp(Result)]() mutable
			{
				if (Handle->IsCancelled() || IsEngineExitRequested())
				{
					return;
				}
				Completion(MoveTemp(Result));
			});
	}

	FString FilePath;
	FSplatCollisionBuildSettings CollisionSettings;
	TSharedRef<FSplatRuntimeAsyncHandle> Handle;
	TUniqueFunction<void(FSplatPreparedDataLoadResult&&)> Completion;
};
} // namespace

TSharedRef<FSplatRuntimeAsyncHandle>
FSplatRuntimeAsync::LoadSplatAssetFromPLYFileAsync(
	const FString& FilePath,
	UObject* Outer,
	FOnSplatAssetLoadCompleted Completion)
{
	const FSplatCollisionBuildSettings CollisionSettings =
		USplatAsset::MakeCollisionBuildSettingsSnapshot();
	TWeakObjectPtr<UObject> WeakOuter = Outer;
	const bool bUseTransientOuter = Outer == nullptr;
	TSharedRef<FSplatRuntimeAsyncHandle> Handle =
		MakeShared<FSplatRuntimeAsyncHandle>();

	(new FAutoDeleteAsyncTask<FSplatPreparedDataLoadTask>(
		FilePath,
		CollisionSettings,
		Handle,
		[Handle, WeakOuter, bUseTransientOuter,
		 Completion = MoveTemp(Completion)](
			FSplatPreparedDataLoadResult&& Result) mutable
		{
			if (Handle->IsCancelled() || IsEngineExitRequested())
			{
				return;
			}

			if (Result.bCancelled)
			{
				return;
			}

			if (!Result.bSuccess)
			{
				Completion.ExecuteIfBound(nullptr, false, Result.ErrorMessage);
				return;
			}

			UObject* EffectiveOuter = bUseTransientOuter
				? GetTransientPackage()
				: WeakOuter.Get();
			if (!EffectiveOuter)
			{
				Completion.ExecuteIfBound(
					nullptr,
					false,
					TEXT("Load target outer is no longer valid."));
				return;
			}

			FString ErrorMessage;
			USplatAsset* Asset = CreateRuntimeSplatAssetFromPreparedData(
				EffectiveOuter,
				MoveTemp(Result.PreparedData),
				ErrorMessage);
			Completion.ExecuteIfBound(Asset, Asset != nullptr, ErrorMessage);
		}))->StartBackgroundTask();

	return Handle;
}

TSharedRef<FSplatRuntimeAsyncHandle>
FSplatRuntimeAsync::SpawnSplatActorFromPLYFileAsync(
	UObject* WorldContextObject,
	const FString& FilePath,
	const FTransform& Transform,
	FOnSplatActorSpawnCompleted Completion)
{
	const FSplatCollisionBuildSettings CollisionSettings =
		USplatAsset::MakeCollisionBuildSettingsSnapshot();
	TWeakObjectPtr<UObject> WeakWorldContextObject = WorldContextObject;
	TSharedRef<FSplatRuntimeAsyncHandle> Handle =
		MakeShared<FSplatRuntimeAsyncHandle>();

	(new FAutoDeleteAsyncTask<FSplatPreparedDataLoadTask>(
		FilePath,
		CollisionSettings,
		Handle,
		[Handle, WeakWorldContextObject, Transform,
		 Completion = MoveTemp(Completion)](
			FSplatPreparedDataLoadResult&& Result) mutable
		{
			if (Handle->IsCancelled() || IsEngineExitRequested())
			{
				return;
			}

			if (Result.bCancelled)
			{
				return;
			}

			if (!Result.bSuccess)
			{
				Completion.ExecuteIfBound(nullptr, false, Result.ErrorMessage);
				return;
			}

			if (!GEngine)
			{
				Completion.ExecuteIfBound(
					nullptr, false, TEXT("Engine is unavailable."));
				return;
			}

			UObject* ContextObject = WeakWorldContextObject.Get();
			if (!ContextObject)
			{
				Completion.ExecuteIfBound(
					nullptr,
					false,
					TEXT("World context is no longer valid."));
				return;
			}

			UWorld* World = GEngine->GetWorldFromContextObject(
				ContextObject,
				EGetWorldErrorMode::ReturnNull);
			if (!World)
			{
				Completion.ExecuteIfBound(
					nullptr,
					false,
					TEXT("Invalid world context."));
				return;
			}

			FString ErrorMessage;
			USplatAsset* Asset = CreateRuntimeSplatAssetFromPreparedData(
				World,
				MoveTemp(Result.PreparedData),
				ErrorMessage);
			if (!Asset)
			{
				Completion.ExecuteIfBound(nullptr, false, ErrorMessage);
				return;
			}

			ASplatActor* Actor =
				World->SpawnActor<ASplatActor>(ASplatActor::StaticClass(), Transform);
			if (!Actor)
			{
				Completion.ExecuteIfBound(
					nullptr,
					false,
					TEXT("Failed to spawn a splat actor."));
				return;
			}

			USplatComponent* SplatComponent =
				Actor->FindComponentByClass<USplatComponent>();
			if (!SplatComponent)
			{
				Actor->Destroy();
				Completion.ExecuteIfBound(
					nullptr,
					false,
					TEXT("Spawned actor is missing its splat component."));
				return;
			}

			SplatComponent->SetSplatAsset(Asset);
			Completion.ExecuteIfBound(Actor, true, FString());
		}))->StartBackgroundTask();

	return Handle;
}