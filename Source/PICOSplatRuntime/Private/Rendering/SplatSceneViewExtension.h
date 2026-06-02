/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

class FRDGBuffer;

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "HAL/CriticalSection.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ScopeLock.h"
#include "SceneViewExtension.h"
#include "SplatSceneProxy.h"
#include "Templates/SharedPointer.h"

namespace PICO::Splat
{

/**
 * Extends the Engine's rendering system to support 3DGS.
 *
 * Splits splat rendering into two phases:
 * 1. Distancing, sorting and projection, which kicks off before rendering the
 * 		current view.
 * 2. Actual rendering, which happens after the base pass or before
 * 	  post-processing, depending on whether the renderer is desktop or mobile.
 */
class FSplatSceneViewExtension final : public FSceneViewExtensionBase
{
public:
	struct FVisibleProxyFrameEntry
	{
		FSplatSceneProxy* Proxy = nullptr;
		uint32 ProxySlot = 0;
		uint32 SplatOffset = 0;
		uint32 NumSplats = 0;
		FMatrix44f LocalToWorld = FMatrix44f::Identity;
		FVector3f PosMinCM = FVector3f::ZeroVector;
		FVector3f PosScaleCM = FVector3f::ZeroVector;
		FShaderResourceViewRHIRef PositionsSRV;
		FShaderResourceViewRHIRef ColorsSRV;
		FShaderResourceViewRHIRef SphericalHarmonicsSRV;
		FShaderResourceViewRHIRef CovariancesSRV;
		FShaderResourceViewRHIRef TransformsSRV;
	};

	struct FGlobalSortFrameData
	{
		// Owning view family pointer, used to scope cleanup so other
		// concurrent families do not wipe each other's prepared data.
		const FSceneViewFamily* OwningFamily = nullptr;
		TArray<FVisibleProxyFrameEntry> VisibleProxies;
		uint32 TotalVisibleSplats = 0;
		bool bGlobalSortRequested = false;
		bool bGlobalSortSupported = false;
		FRDGBuffer* GlobalMetadataPackedIndices = nullptr;
		FRDGBuffer* GlobalMetadataDistances = nullptr;

		void Reset()
		{
			OwningFamily = nullptr;
			VisibleProxies.Reset();
			TotalVisibleSplats = 0;
			bGlobalSortRequested = false;
			bGlobalSortSupported = false;
			GlobalMetadataPackedIndices = nullptr;
			GlobalMetadataDistances = nullptr;
		}
	};

	FSplatSceneViewExtension(const FAutoRegister& AutoRegister);

	//~ Begin ISceneViewExtension Interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {};
	virtual void
	SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {};
	virtual void
	BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void PostRenderViewFamily_RenderThread(
		FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

	/**
	 * First stage: Enqueue async compute work, to be done before actual
	 * rendering.
	 *
	 * 1. Measure distance to each splat (if GPU sort enabled).
	 * 2. Sort splats by distance (if GPU sort enabled).
	 * 3. Project splats (calculate 2x2 transform).
	 */
	virtual void PreRenderView_RenderThread(
		FRDGBuilder& GraphBuilder, FSceneView& InView) override;

	/**
	 * Second stage, on desktop renderer. Transforms and renders splats based on
	 * output from first stage.
	 *
	 * Must occur *after* lighting, as when using deferred rendering,
	 * transparent edges will pull in black from the unlit SceneColor.
	 */
	virtual void PrePostProcessPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessingInputs& Inputs) override;
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

	/**
	 * Second stage, on mobile renderer.
	 */
	virtual void PostRenderBasePassMobile_RenderThread(
		FRHICommandList& RHICmdList, FSceneView& InView) override;
	//~ End ISceneViewExtension Interface

	/**
	 * Registers a splat for rendering. Continues until a subsequent call to
	 * `UnregisterSplat_RenderThread`.
	 *
	 * @param Proxy - The splat to begin rendering.
	 */
	void RegisterSplat_RenderThread(FSplatSceneProxy* Proxy)
	{
		check(IsInRenderingThread());
		if (!Proxy)
		{
			return;
		}
		FScopeLock Lock(&FrameDataCS);
		Proxies.Add(Proxy);
	}

	/**
	 * Stop rendering a splat.
	 *
	 * @param Proxy - The splat to stop rendering.
	 */
	void UnregisterSplat_RenderThread(FSplatSceneProxy* Proxy)
	{
		check(IsInRenderingThread());
		FScopeLock Lock(&FrameDataCS);
		Proxies.Remove(Proxy);
		// Invalidate any cached frame entries that still reference this
		// proxy, so the render passes built earlier this frame skip it
		// instead of dereferencing a soon-to-be-destroyed pointer.
		for (TPair<const FSceneView*, TSharedPtr<FGlobalSortFrameData>>& Pair :
			 FrameDataByView)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}
			for (FVisibleProxyFrameEntry& Entry : Pair.Value->VisibleProxies)
			{
				if (Entry.Proxy == Proxy)
				{
					Entry.Proxy = nullptr;
					Entry.PositionsSRV.SafeRelease();
					Entry.ColorsSRV.SafeRelease();
					Entry.SphericalHarmonicsSRV.SafeRelease();
					Entry.CovariancesSRV.SafeRelease();
					Entry.TransformsSRV.SafeRelease();
				}
			}
		}
	}

private:
	TSharedRef<FGlobalSortFrameData> BuildFrameData_RenderThread(
		const FSceneView& View, bool bSkipNeedsSort);
	TSharedPtr<FGlobalSortFrameData> FindFrameData_RenderThread(
		const FSceneView& View);
	FScreenPassTexture PostTemporalPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	void RenderSplats_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef SceneColorTexture,
		FRDGTextureRef SceneDepthTexture);
	void LogGlobalSortFallbackOnce_RenderThread();

	bool bIsSortingOnGPU;
	bool bHasLoggedGlobalSortFallback;
	// All access to `Proxies` and `FrameDataByView` must be guarded by this
	// critical section. The renderer can dispatch view-extension callbacks
	// for different views (e.g. multiple viewports, scene captures, PIE)
	// concurrently on the render thread / parallel translate tasks, and
	// concurrent mutation otherwise corrupts the TMap/TSet.
	mutable FCriticalSection FrameDataCS;
	// Heap-allocated values keep stable pointers when the map rehashes, so
	// references handed out to passes remain valid even if other views add
	// or remove entries while RDG passes are still being constructed.
	TMap<const FSceneView*, TSharedPtr<FGlobalSortFrameData>> FrameDataByView;
	TSet<FSplatSceneProxy*> Proxies;
};

} // namespace PICO::Splat