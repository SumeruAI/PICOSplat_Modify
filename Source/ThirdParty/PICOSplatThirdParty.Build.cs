/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class PICOSplatThirdParty : ModuleRules
{
	public PICOSplatThirdParty(ReadOnlyTargetRules Target) : base(Target)
	{
		string SplatExportMacro = Target.Platform == UnrealTargetPlatform.Win64
			? "__declspec(dllexport)"
			: "";
		PrivateDefinitions.Add($"SPLAT_EXPORT_API={SplatExportMacro}");
		PrivateDependencyModuleNames.Add("Core");

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "splat"));
	}
}