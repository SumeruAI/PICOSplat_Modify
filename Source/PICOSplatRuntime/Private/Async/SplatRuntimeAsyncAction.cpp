#include "Async/SplatRuntimeAsyncAction.h"

#include "Async/SplatRuntimeAsync.h"
#include "Engine/GameInstance.h"

ULoadSplatAssetFromPLYFileAsyncAction*
ULoadSplatAssetFromPLYFileAsyncAction::LoadSplatAssetFromPLYFileAsync(
	UObject* InWorldContextObject,
	const FString& InFilePath,
	UObject* InOuter)
{
	ULoadSplatAssetFromPLYFileAsyncAction* Action =
		NewObject<ULoadSplatAssetFromPLYFileAsyncAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->Outer = InOuter;
	Action->FilePath = InFilePath;
	Action->bUseTransientOuter = InOuter == nullptr;
	if (InWorldContextObject)
	{
		Action->RegisterWithGameInstance(InWorldContextObject);
	}
	return Action;
}

void ULoadSplatAssetFromPLYFileAsyncAction::Activate()
{
	if (bActivated)
	{
		return;
	}
	bActivated = true;

	if (FilePath.IsEmpty())
	{
		BroadcastFailed(TEXT("File path is empty."));
		return;
	}

	TWeakObjectPtr<ULoadSplatAssetFromPLYFileAsyncAction> WeakThis(this);
	FSplatRuntimeAsync::LoadSplatAssetFromPLYFileAsync(
		FilePath,
		bUseTransientOuter ? nullptr : Outer.Get(),
		FOnSplatAssetLoadCompleted::CreateLambda(
			[WeakThis](USplatAsset* Asset, bool bSuccess, const FString& ErrorMessage)
			{
				if (!WeakThis.IsValid())
				{
					return;
				}

				if (bSuccess && Asset)
				{
					WeakThis->BroadcastCompleted(Asset, ErrorMessage);
					return;
				}

				WeakThis->BroadcastFailed(ErrorMessage);
			}));
}

void ULoadSplatAssetFromPLYFileAsyncAction::BroadcastCompleted(
	USplatAsset* Asset,
	const FString& ErrorMessage)
{
	Completed.Broadcast(Asset, ErrorMessage);
	SetReadyToDestroy();
}

void ULoadSplatAssetFromPLYFileAsyncAction::BroadcastFailed(
	const FString& ErrorMessage)
{
	Failed.Broadcast(nullptr, ErrorMessage);
	SetReadyToDestroy();
}

USpawnSplatActorFromPLYFileAsyncAction*
USpawnSplatActorFromPLYFileAsyncAction::SpawnSplatActorFromPLYFileAsync(
	UObject* InWorldContextObject,
	const FString& InFilePath,
	const FTransform& InTransform)
{
	USpawnSplatActorFromPLYFileAsyncAction* Action =
		NewObject<USpawnSplatActorFromPLYFileAsyncAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->FilePath = InFilePath;
	Action->Transform = InTransform;
	if (InWorldContextObject)
	{
		Action->RegisterWithGameInstance(InWorldContextObject);
	}
	return Action;
}

void USpawnSplatActorFromPLYFileAsyncAction::Activate()
{
	if (bActivated)
	{
		return;
	}
	bActivated = true;

	if (FilePath.IsEmpty())
	{
		BroadcastFailed(TEXT("File path is empty."));
		return;
	}

	TWeakObjectPtr<USpawnSplatActorFromPLYFileAsyncAction> WeakThis(this);
	FSplatRuntimeAsync::SpawnSplatActorFromPLYFileAsync(
		WorldContextObject.Get(),
		FilePath,
		Transform,
		FOnSplatActorSpawnCompleted::CreateLambda(
			[WeakThis](ASplatActor* Actor, bool bSuccess, const FString& ErrorMessage)
			{
				if (!WeakThis.IsValid())
				{
					return;
				}

				if (bSuccess && Actor)
				{
					WeakThis->BroadcastCompleted(Actor, ErrorMessage);
					return;
				}

				WeakThis->BroadcastFailed(ErrorMessage);
			}));
}

void USpawnSplatActorFromPLYFileAsyncAction::BroadcastCompleted(
	ASplatActor* Actor,
	const FString& ErrorMessage)
{
	Completed.Broadcast(Actor, ErrorMessage);
	SetReadyToDestroy();
}

void USpawnSplatActorFromPLYFileAsyncAction::BroadcastFailed(
	const FString& ErrorMessage)
{
	Failed.Broadcast(nullptr, ErrorMessage);
	SetReadyToDestroy();
}