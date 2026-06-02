/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class PICOSplatRuntime : ModuleRules
{
	public PICOSplatRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		string SplatImportMacro = Target.Platform == UnrealTargetPlatform.Win64
			? "__declspec(dllimport)"
			: "";
		PrivateDefinitions.Add($"SPLAT_EXPORT_API={SplatImportMacro}");

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"GeometryCore",
				"PICOSplatThirdParty",
				"Projects",
				"RenderCore",
				"Renderer",
				"RHI",
			}
		);

		PrivateIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(GetModuleDirectory("Renderer"), "Private"),
			}
		);

		// Optional runtime PLY staging path for projects that load splats from
		// loose files instead of baking every asset into a .uasset. Files staged
		// as SystemNonUFS remain visible to native/runtime file I/O in packaged
		// builds.
		if (!Target.bBuildEditor && Target.ProjectFile != null)
		{
			string RuntimePlyRoot = Path.Combine(
				Target.ProjectFile.Directory.FullName,
				"Content",
				"PICOSplat",
				"RuntimePLY");
			if (Directory.Exists(RuntimePlyRoot))
			{
				RuntimeDependencies.Add(
					"$(ProjectDir)/Content/PICOSplat/RuntimePLY/...",
					StagedFileType.SystemNonUFS);
			}
		}
	}
}