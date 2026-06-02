/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatAssetFactory.h"

#include "Import/SplatRuntimeLoader.h"
#include "Logging.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "SplatSettings.h"

USplatAssetFactory::USplatAssetFactory()
{
	SupportedClass = USplatAsset::StaticClass();

	Formats.Emplace(TEXT("ply;Gaussian splat"));
	bEditorImport = true;
}

UObject* USplatAssetFactory::FactoryCreateBinary(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UObject* Context,
	const TCHAR* Type,
	const uint8*& Buffer,
	const uint8* BufferEnd,
	FFeedbackContext* Warn)
{
	PICO_LOGL("Loading splats from %s.", *InName.ToString());
	FScopedSlowTask ImportTask(
		100.0f,
		FText::Format(
			NSLOCTEXT("PICOSplatEditor", "ImportPLYProgress", "Importing {0}"),
			FText::FromName(InName)));
	ImportTask.MakeDialog(true);

	const int64 BufferSize = BufferEnd - Buffer;
	FRuntimeSplatBuildData BuildData;
	FString ErrorMessage;
	ImportTask.EnterProgressFrame(
		35.0f,
		NSLOCTEXT("PICOSplatEditor", "ImportPLYParse", "Parsing PLY data..."));
	if (!FSplatRuntimeLoader::LoadFromPLYMemory(
			MakeArrayView(Buffer, static_cast<int32>(BufferSize)),
			BuildData,
			ErrorMessage))
	{
		PICO_LOGE("Failed to import %s: %s", *InName.ToString(), *ErrorMessage);
		return nullptr;
	}
	if (ImportTask.ShouldCancel())
	{
		return nullptr;
	}

	ImportTask.EnterProgressFrame(
		10.0f,
		NSLOCTEXT("PICOSplatEditor", "ImportPLYAllocate", "Creating splat asset..."));
	USplatAsset* Asset = NewObject<USplatAsset>(InParent, InName, Flags);
	if (!Asset)
	{
		PICO_LOGE("Failed to allocate asset for %s.", *InName.ToString());
		return nullptr;
	}

	Asset->SetSourceFilePath(UFactory::GetCurrentFilename());

	// Optionally cache the unfiltered source data before initialization, so the
	// asset can be rebuilt later without reading the source PLY. Stripped from
	// cooked builds via WITH_EDITORONLY_DATA serialization.
	ImportTask.EnterProgressFrame(
		10.0f,
		NSLOCTEXT("PICOSplatEditor", "ImportPLYSourceData", "Recording source reference..."));
	if (USplatSettings::ShouldKeepImportSourceData())
	{
		Asset->SetSourceImportData(BuildData);
	}

	ImportTask.EnterProgressFrame(
		40.0f,
		NSLOCTEXT("PICOSplatEditor", "ImportPLYBuild", "Building render and collision data..."));
	if (!Asset->InitializeFromRuntimeData(BuildData, ErrorMessage))
	{
		PICO_LOGE("Failed to initialize %s: %s", *InName.ToString(), *ErrorMessage);
		return nullptr;
	}

	ImportTask.EnterProgressFrame(
		5.0f,
		NSLOCTEXT("PICOSplatEditor", "ImportPLYFinish", "Finishing import..."));

	return Asset;
}

bool USplatAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	USplatAsset* Asset = Cast<USplatAsset>(Obj);
	if (!Asset)
	{
		return false;
	}

	if (!Asset->GetSourceFilePath().IsEmpty())
	{
		OutFilenames.Add(Asset->GetSourceFilePath());
	}
	return true;
}

void USplatAssetFactory::SetReimportPaths(
	UObject* Obj,
	const TArray<FString>& NewReimportPaths)
{
	USplatAsset* Asset = Cast<USplatAsset>(Obj);
	if (Asset && NewReimportPaths.Num() > 0)
	{
		Asset->SetSourceFilePath(NewReimportPaths[0]);
	}
}

EReimportResult::Type USplatAssetFactory::Reimport(UObject* Obj)
{
	USplatAsset* Asset = Cast<USplatAsset>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	const FString SourceFilePath = Asset->GetSourceFilePath();
	if (!SourceFilePath.IsEmpty() && !FPaths::FileExists(SourceFilePath))
	{
		UE_LOG(LogPICOSplat, Warning,
			TEXT("Cannot reimport splat asset %s: source PLY file is missing: %s"),
			*Asset->GetPathName(),
			*SourceFilePath);
		return EReimportResult::Failed;
	}
	if (SourceFilePath.IsEmpty() && !Asset->HasSourceData())
	{
		UE_LOG(LogPICOSplat, Warning,
			TEXT("Cannot reimport splat asset %s: no source PLY path or cached source data is available."),
			*Asset->GetPathName());
		return EReimportResult::Failed;
	}

	FString Error;
	if (!Asset->RebuildFromSource(USplatAsset::MakeCollisionBuildSettingsSnapshot(), Error))
	{
		UE_LOG(LogPICOSplat, Warning,
			TEXT("Failed to reimport splat asset %s from %s: %s"),
			*Asset->GetPathName(),
			*SourceFilePath,
			*Error);
		return EReimportResult::Failed;
	}

	Asset->MarkPackageDirty();
	return EReimportResult::Succeeded;
}

int32 USplatAssetFactory::GetPriority() const
{
	return ImportPriority;
}