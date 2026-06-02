/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "SplatAsset.h"
#include "SplatComponent.generated.h"

/**
 * Component holding a renderable 3DGS model or scene.
 *
 * @see https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine
 *
 * TODO(seth): I haven't figured out why the BodyInstance's Physics Actor is not
 * being created successfully on device. Until this is resolved, physics won't
 * work on device.
 */
UCLASS()
class PICOSPLATRUNTIME_API USplatComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual UBodySetup* GetBodySetup() override;
#if WITH_EDITOR
	// Materials are only used in Editor, for mouse selection and debug views.
	virtual void GetUsedMaterials(
		TArray<UMaterialInterface*>& OutMaterials,
		bool bGetDebugMaterials = false) const override;
#endif
	//~ End UPrimitiveComponent Interface

	//~ Begin USceneComponent Interface
	virtual FBoxSphereBounds
	CalcBounds(const FTransform& LocalToWorld) const override;
#if WITH_EDITOR
	virtual bool ShouldCollideWhenPlacing() const override { return true; }
#endif
	//~ End USceneComponent Interface

	/**
	 * Gets the asset this component is tied to, if any.
	 *
	 * @return The asset attached to this component, or nullptr.
	 */
	TObjectPtr<USplatAsset> GetAsset() const { return Asset; }

	/** @return Whether this component forces a single merged convex hull. */
	bool GetForceSingleConvexHull() const { return bForceSingleConvexHull; }

	/**
	 * Updates the splat asset displayed by this component.
	 *
	 * @param InAsset - The asset to render.
	 */
	UFUNCTION(BlueprintCallable, Category = Splat)
	void SetSplatAsset(USplatAsset* InAsset);

	/**
	 * Regenerates the attached asset's collision hulls using the project's
	 * current collision build settings, then refreshes this component's
	 * physics body and editor wireframe. Useful after tuning collision
	 * settings, without re-importing the source PLY.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Splat|Collision")
	void RebuildCollision();

#if WITH_EDITOR
	/**
	 * Rebuilds the attached asset's render and collision data from its cached
	 * source import data, applying the project's current import/collision
	 * settings. Disabled when the asset has no cached source data
	 * (e.g. imported with "Keep Import Source Data" off, or stripped).
	 */
	UFUNCTION(CallInEditor, Category = "Splat|Import")
	void RebuildFromSource();

	/** Removes cached source import data from the attached asset to reduce editor .uasset size. */
	UFUNCTION(CallInEditor, Category = "Splat|Import")
	void StripCachedSourceData();
#endif

private:
	UPROPERTY(Category = Splat, EditAnywhere)
	TObjectPtr<USplatAsset> Asset;

	/** When enabled, merges all convex hulls from the asset into one for this
	 *  component's physics collision. Does not affect other components or the
	 *  asset itself. */
	UPROPERTY(
		Category = Splat,
		EditAnywhere,
		meta = (DisplayName = "Force Single Convex Hull"))
	bool bForceSingleConvexHull = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(
		FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UPROPERTY()
	TObjectPtr<UBodySetup> BodySetup;

#if WITH_EDITOR
	friend class UActorFactorySplat;
#endif
};