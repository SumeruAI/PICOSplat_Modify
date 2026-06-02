/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Logging.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/Object.h"

#include "SplatSettings.generated.h"

UENUM(BlueprintType)
enum class ECovarianceFormat : uint8
{
	Float10 = 0 UMETA(DisplayName = "64 Bits: Float10/11x6"),
	Float16 = 1 UMETA(DisplayName = "128 Bits: Float16x6 + Pad"),
	Float32 = 2 UMETA(DisplayName = "256 Bits: Float32x6 + Pad")
};

UENUM(BlueprintType)
enum class EDepthFormat : uint8
{
	InvertedUInt16 = 0 UMETA(DisplayName = "16 Bits: Inverted UInt16")
};

UENUM(BlueprintType)
enum class EPositionFormat : uint8
{
	UNorm10 = 0 UMETA(DisplayName = "32 Bits: UNorm10/11x3"),
	Float16 = 1 UMETA(DisplayName = "64 Bits: Float16x3 + Pad"),
	Float32 = 2 UMETA(DisplayName = "128 Bits: Float32x3 + Pad")
};

UENUM(BlueprintType)
enum class ESortingMethod : uint8
{
	CPUAsynchronous = 0 UMETA(DisplayName = "CPU Asynchronous"),
	GPUSynchronous = 1 UMETA(DisplayName = "GPU Synchronous"),
};

UENUM(BlueprintType)
enum class ESplatRadius : uint8
{
	TwoSqrt2 = 0 UMETA(DisplayName = "2 * Sqrt(2) σ (Standard)"),
	Three = 1 UMETA(DisplayName = "3 σ"),
};

UENUM(BlueprintType)
enum class EPICOSplatQualityPreset : uint8
{
	Custom = 0 UMETA(DisplayName = "Custom"),
	Performance = 1 UMETA(DisplayName = "Performance"),
	Balanced = 2 UMETA(DisplayName = "Balanced"),
	Quality = 3 UMETA(DisplayName = "Quality"),
	Cinematic = 4 UMETA(DisplayName = "Cinematic"),
};

/**
 * Global settings.
 *
 * @param Config - `= Engine`, saves settings in `Engine.ini`.
 * @param DefaultConfig - Saves settings to default `.ini`s, not local.
 */
UCLASS(Config = Engine, DefaultConfig)
class PICOSPLATRUNTIME_API USplatSettings final : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Helper to check config `.ini` for sorting method.
	 *
	 * @return Whether to use GPU sorting.
	 */
	static bool IsSortingOnGPU()
	{
		FString SortingMethod;
		if (GConfig->GetString(
				TEXT("/Script/PICOSplatRuntime.SplatSettings"),
				TEXT("SortingMethod"),
				SortingMethod,
				GEngineIni))
		{
			const FString NAME_GPU_SYNC(TEXT("GPUSynchronous"));
			const FString NAME_CPU_ASYNC(TEXT("CPUAsynchronous"));
			if (SortingMethod == NAME_GPU_SYNC)
			{
				return true;
			}
			else if (SortingMethod == NAME_CPU_ASYNC)
			{
				return false;
			}
			else
			{
				PICO_LOGE("Unknown sorting method: %s", *SortingMethod);
			}
		}

		return false;
	}

	static float GetCollisionVoxelSizeMeters()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 0.75f;
		case EPICOSplatQualityPreset::Balanced:
			return 0.5f;
		case EPICOSplatQualityPreset::Quality:
			return 0.35f;
		case EPICOSplatQualityPreset::Cinematic:
			return 0.25f;
		default:
			return FMath::Max(0.01f, Settings->CollisionVoxelSizeMeters);
		}
	}

	static int32 GetMinPointsPerCollisionVoxel()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 16;
		case EPICOSplatQualityPreset::Balanced:
			return 8;
		case EPICOSplatQualityPreset::Quality:
			return 6;
		case EPICOSplatQualityPreset::Cinematic:
			return 4;
		default:
			return FMath::Max(1, Settings->MinPointsPerCollisionVoxel);
		}
	}

	static int32 GetMinPointsPerCollisionComponent()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 32;
		case EPICOSplatQualityPreset::Balanced:
			return 16;
		case EPICOSplatQualityPreset::Quality:
			return 8;
		case EPICOSplatQualityPreset::Cinematic:
			return 4;
		default:
			return FMath::Max(4, Settings->MinPointsPerCollisionComponent);
		}
	}

	static int32 GetMaxCollisionHulls()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 4;
		case EPICOSplatQualityPreset::Balanced:
			return 8;
		case EPICOSplatQualityPreset::Quality:
			return 12;
		case EPICOSplatQualityPreset::Cinematic:
			return 16;
		default:
			return FMath::Max(1, Settings->MaxCollisionHulls);
		}
	}

	static float GetCollisionRadiusScale()
	{
		return FMath::Max(0.0f, GetDefault<USplatSettings>()->CollisionRadiusScale);
	}

	static int32 GetMaxCollisionExpansionVoxels()
	{
		return FMath::Max(
			0,
			GetDefault<USplatSettings>()->MaxCollisionExpansionVoxels);
	}

	static float GetCollisionOutlierDistanceFactor()
	{
		return FMath::Max(
			0.0f,
			GetDefault<USplatSettings>()->CollisionOutlierDistanceFactor);
	}

	static float GetCollisionOutlierMaxTrimFraction()
	{
		return FMath::Clamp(
			GetDefault<USplatSettings>()->CollisionOutlierMaxTrimFraction,
			0.0f,
			0.95f);
	}

	static int32 GetMinPointsForCollisionOutlierRejection()
	{
		return FMath::Max(
			4,
			GetDefault<USplatSettings>()->MinPointsForCollisionOutlierRejection);
	}

	static bool ShowColoredCollisionHullsInEditor()
	{
		return GetDefault<USplatSettings>()->bShowColoredCollisionHullsInEditor;
	}

	static bool IsRenderOutlierRejectionEnabled()
	{
		return GetDefault<USplatSettings>()->bEnableRenderOutlierRejection;
	}

	static float GetRenderOutlierMADMultiplier()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 4.0f;
		case EPICOSplatQualityPreset::Balanced:
			return 5.0f;
		case EPICOSplatQualityPreset::Quality:
			return 6.0f;
		case EPICOSplatQualityPreset::Cinematic:
			return 8.0f;
		default:
			return FMath::Max(0.0f, Settings->RenderOutlierMADMultiplier);
		}
	}

	static float GetRenderOutlierMaxTrimFraction()
	{
		const USplatSettings* Settings = GetDefault<USplatSettings>();
		switch (Settings->QualityPreset)
		{
		case EPICOSplatQualityPreset::Performance:
			return 0.3f;
		case EPICOSplatQualityPreset::Balanced:
			return 0.15f;
		case EPICOSplatQualityPreset::Quality:
			return 0.1f;
		case EPICOSplatQualityPreset::Cinematic:
			return 0.05f;
		default:
			return FMath::Clamp(Settings->RenderOutlierMaxTrimFraction, 0.0f, 0.95f);
		}
	}

	static int32 GetMinPointsForRenderOutlierRejection()
	{
		return FMath::Max(
			4,
			GetDefault<USplatSettings>()->MinPointsForRenderOutlierRejection);
	}

	/**
	 * @return Whether the splat asset import path should cache the original
	 * unfiltered splat data so the asset can be rebuilt later without
	 * re-importing the source PLY. Editor-only feature; the cached data is
	 * stripped from cooked builds.
	 */
	static bool ShouldKeepImportSourceData()
	{
		return GetDefault<USplatSettings>()->bKeepImportSourceData;
	}

	static EPICOSplatQualityPreset GetQualityPreset()
	{
		return GetDefault<USplatSettings>()->QualityPreset;
	}

private:
	/**
	 * Specifiers:
	 * @param Category - `= NAME`, section header property grouped under.
	 * @param Config - Saves to `.ini`.
	 * @param EditAnywhere - Editable via UI.
	 *
	 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/property-specifiers?application_version=4.27
	 */

	/** Format used to store covariance (i.e. scaling and rotation) of splats. Larger formats increase asset size, memory usage and time spent reading data in shaders, in exchange for improved visual quality. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta =
			(ConfigRestartRequired = true,
	         DisplayName = "Covariance Format",
	         EditCondition = false))
	ECovarianceFormat CovarianceFormat = ECovarianceFormat::Float10;

	/** Format used for depth values when sorting splats. Higher bit counts may have slightly better results in certain scenes, at an increased performance cost. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta =
			(ConfigRestartRequired = true,
	         DisplayName = "Depth Format",
	         EditCondition = false))
	EDepthFormat DepthFormat = EDepthFormat::InvertedUInt16;

	/** Format used to store position of splats. Larger formats increase asset size, memory usage and time spent reading data in shaders, in exchange for improved visual quality. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta =
			(ConfigRestartRequired = true,
	         DisplayName = "Position Format",
	         EditCondition = false))
	EPositionFormat PositionFormat = EPositionFormat::UNorm10;

	/** How splat sorting is performed. Asynchronous methods will be faster in exchange for a slight (albeit likely no noticeable) decrease in visual fidelity. CPU sorting will generally net a much higher framerate, but use a significant amount of CPU time. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta = (ConfigRestartRequired = true, DisplayName = "Sorting Method"))
	ESortingMethod SortingMethod = ESortingMethod::CPUAsynchronous;

	/** User-facing preset for import and collision defaults. Custom preserves the advanced values below; other modes override the key import/collision getters at runtime. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta = (DisplayName = "Quality Preset"))
	EPICOSplatQualityPreset QualityPreset = EPICOSplatQualityPreset::Custom;

	/** Remove distant reconstruction noise from imported render splats before building GPU buffers. Disable this for intentionally sparse or very large captures. */
	UPROPERTY(
		Category = Import,
		Config,
		EditAnywhere,
		meta = (DisplayName = "Enable Render Outlier Rejection"))
	bool bEnableRenderOutlierRejection = true;

	/** Median absolute deviation multiplier used by render outlier rejection. Higher values keep more splats. */
	UPROPERTY(
		Category = Import,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.0", DisplayName = "Render Outlier MAD Multiplier"))
	float RenderOutlierMADMultiplier = 5.0f;

	/** Upper bound on the fraction of render splats that can be removed. If the filter would remove more than this, it is skipped. */
	UPROPERTY(
		Category = Import,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.0", ClampMax = "0.95", DisplayName = "Render Outlier Max Trim Fraction"))
	float RenderOutlierMaxTrimFraction = 0.15f;

	/** Minimum splat count required before render outlier rejection is attempted. */
	UPROPERTY(
		Category = Import,
		Config,
		EditAnywhere,
		meta = (ClampMin = "4", DisplayName = "Min Points For Render Outlier Rejection"))
	int32 MinPointsForRenderOutlierRejection = 32;

	/** When enabled, importing a splat asset caches the original unfiltered splat data so the asset can be rebuilt with different import/collision settings later. Editor-only; cached data is stripped from cooked builds. Increases .uasset size on disk. */
	UPROPERTY(
		Category = Import,
		Config,
		EditAnywhere,
		meta = (DisplayName = "Keep Import Source Data"))
	bool bKeepImportSourceData = false;

	/** Size of the voxel grid used to cluster splat centers into collision hull candidates. Smaller values can better preserve thin structures, but may produce more hulls. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.01", DisplayName = "Collision Voxel Size (Meters)"))
	float CollisionVoxelSizeMeters = 0.5f;

	/** Minimum number of splat centers required for a voxel to be treated as occupied when generating collision. Higher values remove isolated noise more aggressively. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "1", DisplayName = "Min Points Per Collision Voxel"))
	int32 MinPointsPerCollisionVoxel = 8;

	/** Minimum number of points required for a connected component to generate its own collision hull. Small components below this threshold are discarded as noise. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "4", DisplayName = "Min Points Per Collision Component"))
	int32 MinPointsPerCollisionComponent = 16;

	/** Maximum number of convex hulls generated for a single splat asset. Components are sorted by size, and only the largest ones are kept. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "1", DisplayName = "Max Collision Hulls"))
	int32 MaxCollisionHulls = 8;

	/** Multiplies each splat's stored scale when expanding collision occupancy. Increase this if collision appears too thin compared to the rendered splats. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.0", DisplayName = "Collision Radius Scale"))
	float CollisionRadiusScale = 1.0f;

	/** Caps how many voxels each splat can expand outward per axis during collision generation. This prevents very large splats from merging distant components. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0", DisplayName = "Max Collision Expansion Voxels"))
	int32 MaxCollisionExpansionVoxels = 4;

	/** Controls how aggressively distant points are trimmed from each collision component before building a convex hull. Zero disables this pass. Higher values keep more points. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.0", DisplayName = "Collision Outlier Distance Factor"))
	float CollisionOutlierDistanceFactor = 3.0f;

	/** Upper bound on the fraction of points that can be removed by outlier rejection. If the filter would remove more than this, it is discarded as too aggressive. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "0.0", ClampMax = "0.95", DisplayName = "Collision Outlier Max Trim Fraction"))
	float CollisionOutlierMaxTrimFraction = 0.15f;

	/** Minimum number of points required in a component before outlier rejection is attempted. Small components skip this step to avoid unstable trimming. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (ClampMin = "4", DisplayName = "Min Points For Collision Outlier Rejection"))
	int32 MinPointsForCollisionOutlierRejection = 32;

	/** The distance from the center of each splat, in standard deviations σ, in which to evaluate it. Larger values will improve visual fidelity with diminishing returns, while costing increasingly more time in fragment shading. */
	UPROPERTY(
		Category = Configuration,
		Config,
		EditAnywhere,
		meta =
			(ConfigRestartRequired = true,
	         DisplayName = "Splat Radius",
	         EditCondition = false))
	ESplatRadius SplatRadius = ESplatRadius::TwoSqrt2;

	/** Draw each generated collision hull using a distinct color in editor wireframe view to make clustering easier to inspect. */
	UPROPERTY(
		Category = Collision,
		Config,
		EditAnywhere,
		meta = (DisplayName = "Show Colored Collision Hulls In Editor"))
	bool bShowColoredCollisionHullsInEditor = true;
};