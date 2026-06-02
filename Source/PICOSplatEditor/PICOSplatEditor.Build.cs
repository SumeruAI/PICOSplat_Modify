/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class PICOSplatEditor : ModuleRules
{
	public PICOSplatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		string SplatImportMacro = Target.Platform == UnrealTargetPlatform.Win64
			? "__declspec(dllimport)"
			: "";
		PrivateDefinitions.Add($"SPLAT_EXPORT_API={SplatImportMacro}");
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetDefinition",
				"AssetTools",
				"ContentBrowser",
				"Core",
				"CoreUObject",
				"DesktopPlatform",
				"Engine",
				"GeometryCore",
				"LevelEditor",
				"PICOSplatRuntime",
				"PICOSplatThirdParty",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
			}
		);
	}
}