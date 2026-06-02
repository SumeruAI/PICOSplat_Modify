/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include <optional>

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Math/BoxSphereBounds.h"
#include "Math/Color.h"
#include "Math/Quat.h"
#include "PackedTypes.h"
#include "RenderCommandFence.h"
#include "Rendering/SplatBuffers.h"
#include "UObject/Object.h"

#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

#include "SplatAsset.generated.h"

/**
 * Container for imported 3DGS scene/model data.
 * Owns CPU data, and handles loading and unloading of GPU data.
 *
 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine#staticresources
 */
struct FRuntimeSplatBuildData
{
  static constexpr int32 MaxSphericalHarmonicCoefficients = 15;

	TArray<FVector3f> PositionsMeters;
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> ScalesMeters;
	TArray<FColor> Colors;
	TArray<FVector4f> SphericalHarmonics;
};

struct FSplatCollisionBuildSettings
{
	float CollisionVoxelSizeMeters = 0.5f;
	int32 MinPointsPerCollisionVoxel = 8;
	int32 MinPointsPerCollisionComponent = 4;
	int32 MaxCollisionHulls = 8;
	float CollisionRadiusScale = 1.0f;
	int32 MaxCollisionExpansionVoxels = 0;
	float CollisionOutlierDistanceFactor = 0.0f;
	float CollisionOutlierMaxTrimFraction = 0.95f;
	int32 MinPointsForCollisionOutlierRejection = 4;
	bool bForceSingleConvexHull = false;
	bool bEnableRenderOutlierRejection = true;
	float RenderOutlierMADMultiplier = 5.0f;
	float RenderOutlierMaxTrimFraction = 0.15f;
	int32 MinPointsForRenderOutlierRejection = 32;
};

struct FSplatCollisionHull
{
	TArray<FVector3f> Vertices;
	TArray<uint32> Indices;
};

struct FRuntimeSplatPreparedData
{
	FRuntimeSplatBuildData BuildData;
	TArray<FSplatCollisionHull> CollisionHulls;
	TArray<FVector3f> ConvexHullVertices;
	TArray<uint32> ConvexHullIndices;
};

UCLASS()
class PICOSPLATRUNTIME_API USplatAsset : public UObject
{
	GENERATED_BODY()

public:
	static FSplatCollisionBuildSettings MakeCollisionBuildSettingsSnapshot();

	static bool BuildPreparedRuntimeData(
		FRuntimeSplatBuildData&& InData,
		const FSplatCollisionBuildSettings& InCollisionSettings,
		FRuntimeSplatPreparedData& OutPreparedData,
		FString& OutError);

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	virtual void PostLoad() override; // Loading from disk only.
	virtual void Serialize(FArchive& Ar) override;
	//~ End UObject Interface

	/**
	 * Initializes an asset from raw splat data during runtime.
	 *
	 * @param InData - Raw splat data parsed from a file or memory.
	 * @param OutError - Filled with a failure reason when initialization fails.
	 * @return Whether initialization succeeded.
	 */
	bool
	InitializeFromRuntimeData(const FRuntimeSplatBuildData& InData, FString& OutError);

	bool InitializeFromPreparedRuntimeData(
		FRuntimeSplatPreparedData&& InPreparedData,
		FString& OutError);

	/**
	 * Regenerates this asset's collision hulls from its already-stored
	 * splat positions and per-splat extents, applying the given build
	 * settings. Render data (positions/colors/covariances) is left
	 * untouched.
	 *
	 * @param InCollisionSettings - Settings to use when rebuilding hulls.
	 * @param OutError - Filled with a failure reason on error.
	 * @return Whether the rebuild succeeded.
	 */
	bool RebuildCollisionFromStoredData(
		const FSplatCollisionBuildSettings& InCollisionSettings,
		FString& OutError);

#if WITH_EDITOR
	/**
	 * Rebuilds both render and collision data from the original imported
	 * splat data. Uses cached source arrays when present, otherwise reloads
	 * the PLY from the stored source file path.
	 *
	 * Render-thread resources are flushed and re-initialized in place.
	 * Callers must ensure all SceneProxies referencing this asset have
	 * been marked dirty before invoking this; `USplatComponent::RebuildFromSource`
	 * handles that automatically.
	 *
	 * @param InCollisionSettings - Settings to use when rebuilding.
	 * @param OutError - Filled with a failure reason on error.
	 * @return Whether the rebuild succeeded.
	 */
	bool RebuildFromSource(
		const FSplatCollisionBuildSettings& InCollisionSettings,
		FString& OutError);

	/** @return Whether this asset still has the original imported splat data cached. */
	bool HasSourceData() const
	{
		return SourcePositionsMeters.Num() > 0
			&& SourcePositionsMeters.Num() == SourceRotations.Num()
			&& SourcePositionsMeters.Num() == SourceScalesMeters.Num()
			&& SourcePositionsMeters.Num() == SourceColors.Num();
	}

	/**
	 * Stores a copy of the unfiltered imported splat data so this asset can
	 * be rebuilt later via `RebuildFromSource`. Editor-only.
	 */
	void SetSourceImportData(const FRuntimeSplatBuildData& InSourceData);

	/** Drops the cached source data and marks the asset dirty. Editor-only. */
	void ClearSourceImportData();

	void SetSourceFilePath(const FString& InSourceFilePath);

	bool HasSourceFilePath() const { return !SourceFilePath.IsEmpty(); }
	const FString& GetSourceFilePath() const { return SourceFilePath; }

	/** Removes cached source import data to reduce editor .uasset size. */
	UFUNCTION(CallInEditor, Category = "Splat|Import", meta = (DisplayName = "Strip Cached Source Data"))
	void StripCachedSourceData();
#endif

#if WITH_EDITORONLY_DATA
	/** @return Human-readable import/rebuild status shown in the asset details panel. */
	const FString& GetAssetStatus() const { return AssetStatus; }
#endif

	/**
	 * @return SRV for this asset's colors.
	 */
	FShaderResourceViewRHIRef GetColorsSRV() const
	{
		check(Colors);
		check(Colors->ShaderResourceViewRHI);
		return Colors->ShaderResourceViewRHI;
	}

	FShaderResourceViewRHIRef GetSphericalHarmonicsSRV() const
	{
		check(SphericalHarmonics);
		check(SphericalHarmonics->ShaderResourceViewRHI);
		return SphericalHarmonics->ShaderResourceViewRHI;
	}

	/**
	 * Gets the collision hulls generated for this asset.
	 *
	 * @return Array of convex hulls used to build physics collision.
	 */
	const TArray<FSplatCollisionHull>& GetCollisionHulls() const
	{
		return CollisionHulls;
	}

	/**
	 * Gets the indices of this asset's combined convex hull debug mesh.
	 *
	 * @return Constant view of combined hull indices.
	 */
	TConstArrayView<uint32> GetConvexHullIndices() const
	{
		return ConvexHullIndices;
	}

	/**
	 * Gets the vertices of this asset's combined convex hull debug mesh.
	 *
	 * @return Constant view of combined hull vertices.
	 */
	TConstArrayView<FVector3f> GetConvexHullVertices() const
	{
		return ConvexHullVertices;
	}

	/**
	 * @return SRV for this asset's covariance matrices.
	 */
	FShaderResourceViewRHIRef GetCovariancesSRV() const
	{
		check(CovariancesCM);
		check(CovariancesCM->ShaderResourceViewRHI);
		return CovariancesCM->ShaderResourceViewRHI;
	}

	float GetCovarianceScaleCM2() const
	{
		return CovarianceScaleCM2;
	}

	/**
	 * @return The number of splats in this asset.
	 */
	uint32 GetNumSplats() const { return NumSplats; }

	/**
	 * @return Local-space render bounds derived from imported splat positions.
	 */
	FBoxSphereBounds GetRenderBounds() const
	{
		if (NumSplats == 0)
		{
			return FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0);
		}

		const FVector Center = FVector(0.5f * (PosMinCM + PosMaxCM));
		const FVector Extent = FVector(0.5f * (PosMaxCM - PosMinCM));
		return FBoxSphereBounds(Center, Extent, Extent.Length());
	}

	/**
	 * @return Constant view of this asset's positions.
	 */
	TConstArrayView<FVector3f> GetPositions() const
	{
		return PositionsFullPrecision;
	}

	/**
	 * Gets this assets positions, alongside element-wise minimum and scaling.
	 *
	 * @param OutPosMinCM - Element-wise minimum, in centimeters.
	 * @param OutPosScaleCM - Element-wise scale, in centimeters.
	 * @return SRV for this asset's packed positions.
	 */
	FShaderResourceViewRHIRef
	GetPositionsSRV(FVector3f& OutPosMinCM, FVector3f& OutPosScaleCM) const
	{
		check(Positions);
		check(Positions->ShaderResourceViewRHI);
		OutPosMinCM = PosMinCM;
		OutPosScaleCM = PosScaleCM;
		return Positions->ShaderResourceViewRHI;
	}

	/**
	 * Populates this asset with the given colors.
	 *
	 * @param ColorsLinear - Array of linear, 8-bit-per-channel colors.
	 */
	void SetColorsLinear(TArray<FColor>&& ColorsLinear)
	{
		check(ColorsLinear.Num() == NumSplats);

		TStaticMeshVertexData<FColor> Data;
		Data.Assign(ColorsLinear);
		Colors = PICO::Splat::TSplatStaticBuffer(std::move(Data));
	}

	void SetSphericalHarmonics(TArray<FVector4f>&& InSphericalHarmonics)
	{
		const int32 ExpectedCount = static_cast<int32>(NumSplats) *
			FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients;
		check(InSphericalHarmonics.Num() == ExpectedCount);

		TStaticMeshVertexData<FVector4f> Data;
		Data.Assign(InSphericalHarmonics);
		SphericalHarmonics = PICO::Splat::TSplatStaticBuffer(std::move(Data));
	}

	/**
	 * Populates this asset with covariance matrices describing the given
	 * rotations and scales.
	 *
	 * @param Rotations - Array of rotations, one per splat.
	 * @param ScalesMeters - Array of scales, one per splat, in meters.
	 */
	void SetCovariancesQuatScaleMeters(
		const TArray<FQuat4f>& Rotations,
		const TArray<FVector3f>& ScalesMeters);

	/**
	 * Stores per-splat extents used when generating collision hulls.
	 *
	 * @param InCollisionExtentsMeters - Half extents for each splat, in meters.
	 */
	void SetCollisionExtentsMeters(TArray<FVector3f>&& InCollisionExtentsMeters)
	{
		check(InCollisionExtentsMeters.Num() == NumSplats);
		CollisionExtentsMeters = std::move(InCollisionExtentsMeters);
	}

	/**
	 * Sets the number of splats in the asset.
	 *
	 * @param InNumSplats - Number of splats.
	 */
	void SetNumSplats(uint32 InNumSplats) { NumSplats = InNumSplats; }

	/**
	 * Populates this asset with the given positions. If sorting on CPU, this
	 * buffer will be kept around under the class is destroyed.
	 *
	 * @param PositionsMeters - An array of positions, one per splat, in meters.
	 */
	void SetPositionsMeters(TArray<FVector3f>&& PositionsMeters);

private:
	void UpdateAssetDiagnostics();

	/**
	 * Enqueues RHI initialization for all resources.
	 */
	void BeginInit();

	/**
	 * Creates packed position data from an array of positions. Does not copy or
	 * destroy the given buffer.
	 *
	 * @param PositionsMeters - An array of positions, one per splat, in meters.
	 */
	void SetPositionsMetersInternal(const TArray<FVector3f>& PositionsMeters);

	uint32 NumSplats = 0;

	TArray<FVector3f> PositionsFullPrecision;
	TArray<FVector3f> CollisionExtentsMeters;
	FVector3f PosMinCM;
	FVector3f PosMaxCM;
	FVector3f PosScaleCM;
	float CovarianceScaleCM2 = 1.0f;

	/**
	 * Note: Using optionals as these are not populated until after the
	 * asset is constructed. This way, at least these buffers can always be
	 * valid post-construction.
	 */
	std::optional<PICO::Splat::TSplatStaticBuffer<PICO::Splat::FPackedPos64>>
		Positions;
	std::optional<PICO::Splat::TSplatStaticBuffer<PICO::Splat::FPackedCovMat>>
		CovariancesCM;
	std::optional<PICO::Splat::TSplatStaticBuffer<FColor>> Colors;
	std::optional<PICO::Splat::TSplatStaticBuffer<FVector4f>> SphericalHarmonics;

	TArray<FSplatCollisionHull> CollisionHulls;
	TArray<FVector3f> ConvexHullVertices;
	TArray<uint32> ConvexHullIndices;

#if WITH_EDITORONLY_DATA
	/**
	 * Original (unfiltered) imported splat data, kept editor-only so the
	 * asset can be rebuilt with different import/collision settings without
	 * re-importing the source PLY. Stripped from cooked builds.
	 */
	TArray<FVector3f> SourcePositionsMeters;
	TArray<FQuat4f> SourceRotations;
	TArray<FVector3f> SourceScalesMeters;
	TArray<FColor> SourceColors;
	TArray<FVector4f> SourceSphericalHarmonics;

	UPROPERTY(EditAnywhere, Category = "Splat|Import", meta = (DisplayName = "Source PLY File"))
	FString SourceFilePath;

	UPROPERTY(VisibleAnywhere, Instanced, Category = "Splat|Import")
	TObjectPtr<UAssetImportData> AssetImportData;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (DisplayName = "Source Data Cached"))
	bool bHasCachedSourceData = false;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (DisplayName = "Source Splats"))
	int32 SourceSplatCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (DisplayName = "Current Splats"))
	int32 CurrentSplatCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (DisplayName = "Collision Hulls"))
	int32 CollisionHullCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (DisplayName = "Last Import Settings"))
	FString ImportSettingsSummary;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (MultiLine = true, DisplayName = "Render Precision"))
	FString RenderPrecisionSummary;

	UPROPERTY(VisibleAnywhere, Category = "Splat|Diagnostics", meta = (MultiLine = true, DisplayName = "Asset Status"))
	FString AssetStatus;
#endif

	FRenderCommandFence ReleaseResourcesFence;
	bool bRuntimeInitialized = false;

#if WITH_EDITOR
	friend class USplatAssetFactory;
#endif
};