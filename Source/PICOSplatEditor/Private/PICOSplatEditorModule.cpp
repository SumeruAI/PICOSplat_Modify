/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "ISettingsModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserMenuContexts.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "SplatAsset.h"
#include "SplatComponent.h"
#include "SplatSettings.h"
#include "ToolMenus.h"
#include "Selection.h"
#include "import/splat_logging.h"

void splat_log_recv(Level level, const char* message)
{
	switch (level)
	{
	case Level::ERROR:
	{
		UE_LOG(LogPICOSplat, Error, TEXT("%hs"), message);
		break;
	}
	case Level::WARNING:
	{
		UE_LOG(LogPICOSplat, Warning, TEXT("%hs"), message);
		break;
	}
	}
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): Required by LOCTEXT.
#define LOCTEXT_NAMESPACE "PICOSplatEditor"

namespace PICO::Splat
{

/**
 * Unreal requires a class definition for a module.
 * All Editor-only logic (e.g. `ply` import) lives within this module.
 */
class FPICOSplatEditorModule final : public IModuleInterface
{
	virtual void StartupModule() override
	{
		// Register splat log handler.
		set_log_recv(splat_log_recv);

		// Register settings page.
		ISettingsModule* SettingsModule =
			FModuleManager::GetModulePtr<ISettingsModule>("Settings");
		if (SettingsModule)
		{
			SettingsModule->RegisterSettings(
				"Project",
				"Plugins",
				"PICO Splat",
				LOCTEXT("RuntimeSettingsName", "PICO Splat"),
				LOCTEXT(
					"RuntimeSettingsDescription", "PICO Splat configuration."),
				GetMutableDefault<USplatSettings>());
		}

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(
				this,
				&FPICOSplatEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);

		ISettingsModule* SettingsModule =
			FModuleManager::GetModulePtr<ISettingsModule>("Settings");
		if (SettingsModule)
		{
			SettingsModule->UnregisterSettings(
				"Project", "Plugins", "PICO Splat");
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		RegisterSplatAssetContextMenu();

		if (UToolMenu* ToolsMenu =
				UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools"))
		{
			FToolMenuSection& Section = ToolsMenu->FindOrAddSection("PICOSplat");
			Section.Label = LOCTEXT("PICOSplatMenuSection", "PICO Splat");
			AddPICOSplatMenuEntries(Section, false);
		}

		if (UToolMenu* ToolbarMenu =
				UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar"))
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PICOSplat");
			Section.Label = LOCTEXT("PICOSplatToolbarSection", "PICO Splat");
			AddPICOSplatMenuEntries(Section, true);
		}
	}

	void RegisterSplatAssetContextMenu()
	{
		UToolMenu* AssetMenu =
			UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(USplatAsset::StaticClass());
		if (!AssetMenu)
		{
			return;
		}

		FToolMenuSection& Section = AssetMenu->FindOrAddSection("GetAssetActions");
		Section.AddDynamicEntry(
			"PICOSplatReimportActions",
			FNewToolMenuSectionDelegate::CreateRaw(
				this,
				&FPICOSplatEditorModule::PopulateSplatAssetContextMenu));
	}

	void PopulateSplatAssetContextMenu(FToolMenuSection& Section)
	{
		const UContentBrowserAssetContextMenuContext* Context =
			UContentBrowserAssetContextMenuContext::FindContextWithAssets(Section);
		if (!Context || Context->GetSelectedAssetsOfType(USplatAsset::StaticClass()).IsEmpty())
		{
			return;
		}

		FToolUIAction ReimportAction;
		ReimportAction.ExecuteAction = FToolMenuExecuteAction::CreateRaw(
			this,
			&FPICOSplatEditorModule::ReimportSelectedSplatAssets,
			false);
		Section.AddMenuEntry(
			"PICOSplatReimport",
			LOCTEXT("PICOSplatReimport", "Reimport"),
			LOCTEXT("PICOSplatReimportTooltip", "Reimport selected Splat assets from their stored source PLY paths."),
			FSlateIcon(),
			ReimportAction);

		FToolUIAction ReimportWithNewFileAction;
		ReimportWithNewFileAction.ExecuteAction = FToolMenuExecuteAction::CreateRaw(
			this,
			&FPICOSplatEditorModule::ReimportSelectedSplatAssets,
			true);
		Section.AddMenuEntry(
			"PICOSplatReimportWithNewFile",
			LOCTEXT("PICOSplatReimportWithNewFile", "Reimport With New File..."),
			LOCTEXT("PICOSplatReimportWithNewFileTooltip", "Choose a new source PLY file, store it on the Splat asset, and reimport."),
			FSlateIcon(),
			ReimportWithNewFileAction);
	}

	void ReimportSelectedSplatAssets(const FToolMenuContext& MenuContext, bool bForceNewFile)
	{
		const UContentBrowserAssetContextMenuContext* Context =
			UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
		if (!Context)
		{
			return;
		}

		TArray<USplatAsset*> SplatAssets = Context->LoadSelectedObjects<USplatAsset>();
		for (USplatAsset* Asset : SplatAssets)
		{
			if (!Asset)
			{
				continue;
			}

			FReimportManager::Instance()->Reimport(
				Asset,
				true,
				true,
				TEXT(""),
				nullptr,
				INDEX_NONE,
				bForceNewFile);
		}
	}

	void AddPICOSplatMenuEntries(FToolMenuSection& Section, bool bToolbar)
	{
		const auto AddEntry = [&](const FName Name,
			const FText& Label,
			const FText& Tooltip,
			FExecuteAction Action)
		{
			FUIAction UIAction(Action);
			if (bToolbar)
			{
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(
					Name,
					UIAction,
					Label,
					Tooltip,
					FSlateIcon()));
			}
			else
			{
				Section.AddEntry(FToolMenuEntry::InitMenuEntry(
					Name,
					Label,
					Tooltip,
					FSlateIcon(),
					UIAction));
			}
		};

		AddEntry(
			"PICOSplatImportSinglePLY",
			LOCTEXT("PICOSplatImportSinglePLY", "Import Single PLY"),
			LOCTEXT("PICOSplatImportSinglePLYTooltip", "Import one Gaussian splat PLY into /Game/PICOSplat/Imported."),
			FExecuteAction::CreateRaw(this, &FPICOSplatEditorModule::ImportSinglePLY));
		AddEntry(
			"PICOSplatImportPLYSequence",
			LOCTEXT("PICOSplatImportPLYSequence", "Import PLY Sequence"),
			LOCTEXT("PICOSplatImportPLYSequenceTooltip", "Batch import multiple PLY files into a sequence folder."),
			FExecuteAction::CreateRaw(this, &FPICOSplatEditorModule::ImportPLYSequence));
		AddEntry(
			"PICOSplatRebuildSelected",
			LOCTEXT("PICOSplatRebuildSelected", "Rebuild Selected From Source"),
			LOCTEXT("PICOSplatRebuildSelectedTooltip", "Rebuild selected Splat assets or selected actors' SplatComponents from cached source data."),
			FExecuteAction::CreateRaw(this, &FPICOSplatEditorModule::RebuildSelectedFromSource));
		AddEntry(
			"PICOSplatToggleStats",
			LOCTEXT("PICOSplatToggleStats", "Toggle stat PICOSplat"),
			LOCTEXT("PICOSplatToggleStatsTooltip", "Toggle the PICOSplat stats overlay."),
			FExecuteAction::CreateRaw(this, &FPICOSplatEditorModule::ToggleStats));
		AddEntry(
			"PICOSplatOpenValidationChecklist",
			LOCTEXT("PICOSplatOpenValidationChecklist", "Open Validation Checklist"),
			LOCTEXT("PICOSplatOpenValidationChecklistTooltip", "Open the PICOSplat validation checklist document."),
			FExecuteAction::CreateRaw(this, &FPICOSplatEditorModule::OpenValidationChecklist));
	}

	static FString DefaultImportPath()
	{
		return TEXT("/Game/PICOSplat/Imported");
	}

	static bool PickPLYFiles(bool bAllowMultiple, TArray<FString>& OutFiles)
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			return false;
		}

		const void* ParentWindowHandle = nullptr;
		if (FSlateApplication::IsInitialized())
		{
			ParentWindowHandle =
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		}

		const uint32 DialogFlags = bAllowMultiple ? EFileDialogFlags::Multiple : 0;
		return DesktopPlatform->OpenFileDialog(
			const_cast<void*>(ParentWindowHandle),
			bAllowMultiple ? TEXT("Import Gaussian Splat PLY Sequence") : TEXT("Import Gaussian Splat PLY"),
			FPaths::ProjectContentDir(),
			TEXT(""),
			TEXT("PLY files (*.ply)|*.ply"),
			DialogFlags,
			OutFiles);
	}

	static void ImportPLYFiles(const TArray<FString>& Files, const FString& DestinationPath)
	{
		if (Files.IsEmpty())
		{
			return;
		}

		IAssetTools& AssetTools =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		AssetTools.ImportAssets(Files, DestinationPath);
	}

	void ImportSinglePLY()
	{
		TArray<FString> Files;
		if (PickPLYFiles(false, Files))
		{
			ImportPLYFiles(Files, DefaultImportPath());
		}
	}

	void ImportPLYSequence()
	{
		TArray<FString> Files;
		if (!PickPLYFiles(true, Files))
		{
			return;
		}

		FString DestinationPath = DefaultImportPath();
		if (!Files.IsEmpty())
		{
			const FString SequenceName = ObjectTools::SanitizeObjectName(
				FPaths::GetBaseFilename(FPaths::GetPath(Files[0])));
			if (!SequenceName.IsEmpty())
			{
				DestinationPath /= SequenceName;
			}
		}

		ImportPLYFiles(Files, DestinationPath);
	}

	void RebuildSelectedFromSource()
	{
		int32 RebuiltCount = 0;
		if (GEditor)
		{
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (!Actor)
				{
					continue;
				}
				TArray<USplatComponent*> Components;
				Actor->GetComponents<USplatComponent>(Components);
				for (USplatComponent* Component : Components)
				{
					if (Component && Component->GetAsset() &&
					    (Component->GetAsset()->HasSourceData() || Component->GetAsset()->HasSourceFilePath()))
					{
						Component->RebuildFromSource();
						++RebuiltCount;
					}
				}
			}

			TArray<UObject*> SelectedAssets;
			GEditor->GetSelectedObjects()->GetSelectedObjects(
				USplatAsset::StaticClass(), SelectedAssets);
			for (UObject* Object : SelectedAssets)
			{
				USplatAsset* Asset = Cast<USplatAsset>(Object);
				if (!Asset || (!Asset->HasSourceData() && !Asset->HasSourceFilePath()))
				{
					continue;
				}

				FString Error;
				if (Asset->RebuildFromSource(
						USplatAsset::MakeCollisionBuildSettingsSnapshot(), Error))
				{
					Asset->MarkPackageDirty();
					++RebuiltCount;
				}
				else
				{
					UE_LOG(LogPICOSplat, Warning,
						TEXT("Rebuild selected asset failed for %s: %s"),
						*Asset->GetPathName(), *Error);
				}
			}
		}

		UE_LOG(LogPICOSplat, Log,
			TEXT("Rebuild Selected From Source completed for %d item(s)."),
			RebuiltCount);
	}

	void ToggleStats()
	{
		if (GEditor)
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			GEditor->Exec(World, TEXT("stat PICOSplat"));
		}
	}

	void OpenValidationChecklist()
	{
		const FString ChecklistPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Docs/picosplat-validation-checklist.md")));
		if (FPaths::FileExists(ChecklistPath))
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*ChecklistPath);
		}
		else
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::Format(
					LOCTEXT("PICOSplatValidationChecklistMissing", "Validation checklist not found:\n{0}"),
					FText::FromString(ChecklistPath)));
		}
	}
};

} // namespace PICO::Splat

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(PICO::Splat::FPICOSplatEditorModule, PICOSplatEditor);