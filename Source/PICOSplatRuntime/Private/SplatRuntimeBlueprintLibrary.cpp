#include "SplatRuntimeBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Import/SplatRuntimeLoader.h"
#include "SplatActor.h"
#include "SplatAsset.h"
#include "SplatComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

USplatAsset* USplatRuntimeBlueprintLibrary::LoadSplatAssetFromPLYFile(
	const FString& FilePath,
	UObject* Outer,
	bool& bSuccess,
	FString& ErrorMessage)
{
	bSuccess = false;
	ErrorMessage.Reset();

	if (!Outer)
	{
		Outer = GetTransientPackage();
	}

	FRuntimeSplatBuildData BuildData;
	if (!FSplatRuntimeLoader::LoadFromPLYFile(FilePath, BuildData, ErrorMessage))
	{
		return nullptr;
	}

	USplatAsset* Asset = NewObject<USplatAsset>(Outer);
	if (!Asset)
	{
		ErrorMessage = TEXT("Failed to allocate a splat asset.");
		return nullptr;
	}

	if (!Asset->InitializeFromRuntimeData(BuildData, ErrorMessage))
	{
		return nullptr;
	}

	bSuccess = true;
	return Asset;
}

ASplatActor* USplatRuntimeBlueprintLibrary::SpawnSplatActorFromPLYFile(
	UObject* WorldContextObject,
	const FString& FilePath,
	const FTransform& Transform,
	bool& bSuccess,
	FString& ErrorMessage)
{
	bSuccess = false;
	ErrorMessage.Reset();

	if (!GEngine)
	{
		ErrorMessage = TEXT("Engine is unavailable.");
		return nullptr;
	}

	UWorld* World = GEngine->GetWorldFromContextObject(
		WorldContextObject, EGetWorldErrorMode::ReturnNull);
	if (!World)
	{
		ErrorMessage = TEXT("Invalid world context.");
		return nullptr;
	}

	USplatAsset* Asset =
		LoadSplatAssetFromPLYFile(FilePath, World, bSuccess, ErrorMessage);
	if (!bSuccess || !Asset)
	{
		return nullptr;
	}

	ASplatActor* Actor =
		World->SpawnActor<ASplatActor>(ASplatActor::StaticClass(), Transform);
	if (!Actor)
	{
		bSuccess = false;
		ErrorMessage = TEXT("Failed to spawn a splat actor.");
		return nullptr;
	}

	USplatComponent* SplatComponent = Actor->FindComponentByClass<USplatComponent>();
	if (!SplatComponent)
	{
		Actor->Destroy();
		bSuccess = false;
		ErrorMessage = TEXT("Spawned actor is missing its splat component.");
		return nullptr;
	}

	SplatComponent->SetSplatAsset(Asset);
	bSuccess = true;
	return Actor;
}
