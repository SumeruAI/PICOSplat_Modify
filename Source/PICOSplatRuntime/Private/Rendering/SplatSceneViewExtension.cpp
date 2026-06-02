/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatSceneViewExtension.h"

#include "HAL/IConsoleManager.h"
#include "Logging.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PostProcess/PostProcessing.h"
#include "RHICommandList.h"
#include "ScreenPass.h"
#include "SplatRendering.h"
#include "SplatRenderingUtilities.h"
#include "SplatSettings.h"
#include "Stats/Stats.h"
#include "StereoRendering.h"
#include "Templates/UnrealTemplate.h"

DECLARE_STATS_GROUP(TEXT("PICOSplat"), STATGROUP_PICOSplat, STATCAT_Advanced);

DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Registered Proxies"),
	STAT_PICOSplat_RegisteredProxies, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Visible Proxies (PreRenderView)"),
	STAT_PICOSplat_VisibleProxiesPreRender, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Visible Proxies (PrePostProcess)"),
	STAT_PICOSplat_VisibleProxiesPrePost, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drawable Splats (Global)"),
	STAT_PICOSplat_DrawableSplatsGlobal, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drawable Splats (Legacy)"),
	STAT_PICOSplat_DrawableSplatsLegacy, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Global Sort Frames"),
	STAT_PICOSplat_GlobalSortFrames, STATGROUP_PICOSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Global Sort Fallback Frames"),
	STAT_PICOSplat_GlobalSortFallbackFrames, STATGROUP_PICOSplat);
DECLARE_CYCLE_STAT(TEXT("BuildFrameData"),
	STAT_PICOSplat_BuildFrameData, STATGROUP_PICOSplat);
DECLARE_CYCLE_STAT(TEXT("PreRenderView"),
	STAT_PICOSplat_PreRenderView, STATGROUP_PICOSplat);
DECLARE_CYCLE_STAT(TEXT("PrePostProcessPass"),
	STAT_PICOSplat_PrePostProcessPass, STATGROUP_PICOSplat);

namespace PICO::Splat
{
namespace
{
#define PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Params, Suffix, PositionSRV, TransformSRV, ColorSRV) \
	(Params).Positions##Suffix = (PositionSRV); \
	(Params).Transforms##Suffix = (TransformSRV); \
	(Params).Colors##Suffix = (ColorSRV)

TAutoConsoleVariable<int32> CVarPICOSplatGlobalSort(
	TEXT("r.PICOSplat.GlobalSort"),
	1,
	TEXT("Enable global cross-actor splat sorting.\n")
	TEXT("0: Disabled\n")
	TEXT("1: Enabled (default); falls back to the legacy per-proxy renderer if unsupported for the current view/platform)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPICOSplatGlobalSortMaxProxies(
	TEXT("r.PICOSplat.GlobalSortMaxProxies"),
	 int32(Shaders::GLOBAL_RENDER_MAX_PROXIES),
	TEXT("Effective per-view cap on visible splat proxies that may use the ")
	TEXT("global sort path. Clamped to the compile-time shader array size ")
	TEXT("(GLOBAL_RENDER_MAX_PROXIES). Lowering helps test the legacy fallback ")
	TEXT("path; raising requires shader changes and will have no effect."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarPICOSplatRenderSplatScale(
	TEXT("r.PICOSplat.RenderSplatScale"),
	1.0f,
	TEXT("Screen-space scale applied to every rendered Gaussian splat. ")
	TEXT("Increase above 1.0 to close visible gaps in sparse captures; ")
	TEXT("values around 1.15-1.5 are usually enough."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPICOSplatPostTemporalComposite(
	TEXT("r.PICOSplat.PostTemporalComposite"),
	0,
	TEXT("Render splats after UE's temporal upscaler / TAA history and before tonemapping.\n")
	TEXT("0: Use the stable legacy PrePostProcessPass injection point (default).\n")
	TEXT("1: Experimental; composite after the MotionBlur post-process hook."),
	ECVF_RenderThreadSafe);

float GetRenderSplatScale_RenderThread()
{
	return FMath::Max(
		0.01f,
		CVarPICOSplatRenderSplatScale.GetValueOnRenderThread());
}

bool IsPostTemporalCompositeEnabled_RenderThread()
{
	return CVarPICOSplatPostTemporalComposite.GetValueOnRenderThread() != 0;
}

/**
 * See comment in SplatRendering.cpp.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FCPUSortRenderProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, IndicesUAV)
END_SHADER_PARAMETER_STRUCT()

Shaders::FRenderSplatSharedParameters
SetSharedParameters(const FSceneView& View, FSplatSceneProxy* Proxy)
{
	check(Proxy);

	Shaders::FRenderSplatSharedParameters Params;
	Params.View = View.ViewUniformBuffer;
	Params.InstancedView = View.GetInstancedViewUniformBuffer();
	Params.local_to_world = FMatrix44f(Proxy->GetLocalToWorld());
	const FVector ViewOriginCM(GetOrigin(View));
	Params.view_origin_local_cm = FVector3f(
		Proxy->GetLocalToWorld().InverseTransformPosition(ViewOriginCM));
	Params.splat_scale = GetRenderSplatScale_RenderThread();
	Params.bUseSphericalHarmonics =
		GetSphericalHarmonicsShaderFlag_RenderThread();
	Params.spherical_harmonic_coeff_count =
		GetSphericalHarmonicsCoeffCount_RenderThread();
	Params.Positions = MakePositionParams(Proxy);
	Params.transforms = Proxy->GetTransformsSRV();
	Params.colors = Proxy->GetColorsSRV();
	Params.spherical_harmonics = Proxy->GetSphericalHarmonicsSRV();

	return Params;
}

bool CompareProxyDepth(
	const FSceneView& View,
	const FSplatSceneProxy& A,
	const FSplatSceneProxy& B)
{
	const FVector3f ViewOrigin = GetOrigin(View);
	const FVector3f ViewForward = GetForward(View);
	const FVector3f OriginA = FVector3f(A.GetLocalToWorld().GetOrigin());
	const FVector3f OriginB = FVector3f(B.GetLocalToWorld().GetOrigin());
	const float DepthA = FVector3f::DotProduct(OriginA - ViewOrigin, ViewForward);
	const float DepthB = FVector3f::DotProduct(OriginB - ViewOrigin, ViewForward);

	if (!FMath::IsNearlyEqual(DepthA, DepthB))
	{
		return DepthA > DepthB;
	}

	return reinterpret_cast<UPTRINT>(&A) < reinterpret_cast<UPTRINT>(&B);
}

TArray<FSplatSceneViewExtension::FVisibleProxyFrameEntry>
CollectVisibleProxyEntries(
	const TSet<FSplatSceneProxy*>& Proxies,
	const FSceneView& View,
	bool bSkipNeedsSort)
{
	TArray<FSplatSceneViewExtension::FVisibleProxyFrameEntry> VisibleProxies;
	VisibleProxies.Reserve(Proxies.Num());

	for (FSplatSceneProxy* Proxy : Proxies)
	{
		if (!Proxy)
		{
			continue;
		}

		if (!Proxy->IsVisible(View))
		{
			continue;
		}
		if (bSkipNeedsSort && Proxy->NeedsSort())
		{
			continue;
		}

		FSplatSceneViewExtension::FVisibleProxyFrameEntry& Entry =
			VisibleProxies.Emplace_GetRef();
		Entry.Proxy = Proxy;
		Entry.NumSplats = Proxy->GetNumSplats();
		Entry.LocalToWorld = FMatrix44f(Proxy->GetLocalToWorld());
		Entry.PositionsSRV =
			Proxy->GetPositionsSRV(Entry.PosMinCM, Entry.PosScaleCM);
		Entry.ColorsSRV = Proxy->GetColorsSRV();
		Entry.SphericalHarmonicsSRV = Proxy->GetSphericalHarmonicsSRV();
		Entry.CovariancesSRV = Proxy->GetCovariancesSRV();
		if (!bSkipNeedsSort || !Proxy->NeedsSort())
		{
			Entry.TransformsSRV = Proxy->GetTransformsSRV();
		}
	}

	VisibleProxies.Sort(
		[&View](
			const FSplatSceneViewExtension::FVisibleProxyFrameEntry& A,
			const FSplatSceneViewExtension::FVisibleProxyFrameEntry& B)
		{
			if (!A.Proxy || !B.Proxy)
			{
				return A.Proxy != nullptr;
			}
			return CompareProxyDepth(View, *A.Proxy, *B.Proxy);
		});

	return VisibleProxies;
}

void SetGlobalProxyResources(
	Shaders::FRenderGlobalSplatVS::FParameters& Parameters,
	uint32 Slot,
	FRHIShaderResourceView* PositionsSRV,
	FRHIShaderResourceView* TransformsSRV,
	FRHIShaderResourceView* ColorsSRV)
{
	switch (Slot)
	{
	case 0: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 0, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 1: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 1, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 2: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 2, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 3: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 3, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 4: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 4, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 5: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 5, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 6: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 6, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 7: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 7, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 8: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 8, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 9: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 9, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 10: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 10, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 11: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 11, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 12: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 12, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 13: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 13, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 14: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 14, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 15: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 15, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 16: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 16, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 17: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 17, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 18: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 18, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 19: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 19, PositionsSRV, TransformsSRV, ColorsSRV); break;
	case 20: PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES(Parameters, 20, PositionsSRV, TransformsSRV, ColorsSRV); break;
	default: break;
	}
}
} // namespace

FSplatSceneViewExtension::FSplatSceneViewExtension(
	const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
	, bIsSortingOnGPU(USplatSettings::IsSortingOnGPU())
	, bHasLoggedGlobalSortFallback(false)
	, FrameDataByView()
	, Proxies()
{
	FSceneViewExtensionIsActiveFunctor IsActiveFunctor;
	IsActiveFunctor.IsActiveFunction =
		[](const ISceneViewExtension* SceneViewExtension,
	       const FSceneViewExtensionContext& Context)
	{
		// NOTE: `Proxies` is mutated on the render thread, so reading it
		// from the game thread here is racy and can briefly return zero
		// while a proxy is being recreated (e.g. drag/drop). That would
		// disable the extension for a frame and cause splats to disappear
		// for one frame -> visible flicker. Always returning active is
		// safe: when there are no proxies, the inner loops simply iterate
		// nothing.
		return TOptional<bool>(true);
	};

	IsActiveThisFrameFunctions.Add(IsActiveFunctor);
}

void FSplatSceneViewExtension::BeginRenderViewFamily(
	FSceneViewFamily& InViewFamily)
{
	// Intentionally empty: in the editor multiple view families (main
	// viewport, scene captures, asset thumbnails, asset previews) interleave
	// on the render thread, so wiping the whole map here would discard
	// PreRenderView output for a *different* family before its
	// PrePostProcessPass got a chance to consume it -> one-frame splat
	// disappearance / flicker. Cleanup is done per-family in
	// PostRenderViewFamily_RenderThread instead.
}

void FSplatSceneViewExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FScopeLock Lock(&FrameDataCS);
	for (auto It = FrameDataByView.CreateIterator(); It; ++It)
	{
		const TSharedPtr<FGlobalSortFrameData>& Slot = It.Value();
		if (!Slot.IsValid() || Slot->OwningFamily == &InViewFamily)
		{
			It.RemoveCurrent();
		}
	}
}

TSharedRef<FSplatSceneViewExtension::FGlobalSortFrameData>
FSplatSceneViewExtension::BuildFrameData_RenderThread(
	const FSceneView& View, bool bSkipNeedsSort)
{
	SCOPE_CYCLE_COUNTER(STAT_PICOSplat_BuildFrameData);
	FScopeLock Lock(&FrameDataCS);
	TSharedPtr<FGlobalSortFrameData>& Slot = FrameDataByView.FindOrAdd(&View);
	if (!Slot.IsValid())
	{
		Slot = MakeShared<FGlobalSortFrameData>();
	}
	TSharedRef<FGlobalSortFrameData> FrameDataRef = Slot.ToSharedRef();
	FGlobalSortFrameData& FrameData = *FrameDataRef;
	FrameData.Reset();
	FrameData.OwningFamily = View.Family;
	FrameData.bGlobalSortRequested =
		CVarPICOSplatGlobalSort.GetValueOnRenderThread() != 0;
	FrameData.VisibleProxies =
		CollectVisibleProxyEntries(Proxies, View, bSkipNeedsSort);
	const int32 EffectiveMaxProxies = FMath::Clamp(
		CVarPICOSplatGlobalSortMaxProxies.GetValueOnRenderThread(),
		1,
		int32(Shaders::GLOBAL_RENDER_MAX_PROXIES));
	// Platform capability check: the global path relies on async compute and
	// SM5-class compute. Mobile / feature-level downgrades fall back to the
	// legacy per-proxy renderer transparently.
	const bool bPlatformSupportsGlobalSort =
		View.GetFeatureLevel() >= ERHIFeatureLevel::SM5 &&
		GEnableAsyncCompute;
	FrameData.bGlobalSortSupported = FrameData.bGlobalSortRequested &&
		bPlatformSupportsGlobalSort &&
		FrameData.VisibleProxies.Num() <= EffectiveMaxProxies;

	SET_DWORD_STAT(STAT_PICOSplat_RegisteredProxies, Proxies.Num());

	for (int32 Index = 0; Index < FrameData.VisibleProxies.Num(); ++Index)
	{
		FVisibleProxyFrameEntry& Entry = FrameData.VisibleProxies[Index];
		checkf(
			static_cast<uint32>(Index) < Shaders::GLOBAL_SORT_MAX_PROXIES,
			TEXT("Global splat sorting currently supports up to %u visible proxies per view."),
			Shaders::GLOBAL_SORT_MAX_PROXIES);
		checkf(
			Entry.NumSplats <= Shaders::GLOBAL_SORT_MAX_LOCAL_INDEX + 1,
			TEXT("Global splat sorting currently supports up to %u splats per proxy."),
			Shaders::GLOBAL_SORT_MAX_LOCAL_INDEX + 1);
		Entry.ProxySlot = static_cast<uint32>(Index);
		Entry.SplatOffset = FrameData.TotalVisibleSplats;
		FrameData.TotalVisibleSplats += Entry.NumSplats;
	}

	return FrameDataRef;
}

TSharedPtr<FSplatSceneViewExtension::FGlobalSortFrameData>
FSplatSceneViewExtension::FindFrameData_RenderThread(const FSceneView& View)
{
	FScopeLock Lock(&FrameDataCS);
	TSharedPtr<FGlobalSortFrameData>* Slot = FrameDataByView.Find(&View);
	return Slot ? *Slot : nullptr;
}

void FSplatSceneViewExtension::LogGlobalSortFallbackOnce_RenderThread()
{
	if (bHasLoggedGlobalSortFallback)
	{
		return;
	}

	bHasLoggedGlobalSortFallback = true;
	const int32 EffectiveMaxProxies = FMath::Clamp(
		CVarPICOSplatGlobalSortMaxProxies.GetValueOnRenderThread(),
		1,
		int32(Shaders::GLOBAL_RENDER_MAX_PROXIES));
	PICO_LOGW(
		"r.PICOSplat.GlobalSort is enabled, but the current view exceeds the supported limits (effective max proxies = %d, hard shader cap = %u). Falling back to the legacy per-proxy renderer for this frame.",
		EffectiveMaxProxies,
		Shaders::GLOBAL_RENDER_MAX_PROXIES);
}

void FSplatSceneViewExtension::PreRenderView_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& View)
{
	SCOPE_CYCLE_COUNTER(STAT_PICOSplat_PreRenderView);
	/**
	 * Full & primary passes do actual splat calculations, which are shared with
	 * secondary passes (if applicable).
	 *
	 * Full pass: Non-stereo.
	 * Primary: First eye, or both (e.g. instanced stereo or multiview).
	 * Secondary: Second eye.
	 */
	if (IStereoRendering::IsASecondaryView(View))
	{
		return;
	}

	TSharedRef<FGlobalSortFrameData> FrameDataRef =
		BuildFrameData_RenderThread(View, false);
	FGlobalSortFrameData& FrameData = *FrameDataRef;
	INC_DWORD_STAT_BY(STAT_PICOSplat_VisibleProxiesPreRender,
		FrameData.VisibleProxies.Num());
	if (FrameData.bGlobalSortRequested && !FrameData.bGlobalSortSupported)
	{
		LogGlobalSortFallbackOnce_RenderThread();
	}
	const bool bUseGlobalSort = FrameData.bGlobalSortRequested &&
		FrameData.bGlobalSortSupported;
	if (bUseGlobalSort &&
	    FrameData.TotalVisibleSplats > 0)
	{
		FRDGBufferDesc MetadataIndexDesc = FRDGBufferDesc::CreateBufferDesc(
			sizeof(uint32), FrameData.TotalVisibleSplats);
		FrameData.GlobalMetadataPackedIndices = GraphBuilder.CreateBuffer(
			MetadataIndexDesc, TEXT("GlobalSplatMetadataPackedIndices"));

		FRDGBufferDesc MetadataDistanceDesc =
			FRDGBufferDesc::CreateBufferDesc(
				sizeof(uint16), FrameData.TotalVisibleSplats);
		FrameData.GlobalMetadataDistances = GraphBuilder.CreateBuffer(
			MetadataDistanceDesc, TEXT("GlobalSplatMetadataDistances"));

		for (const FVisibleProxyFrameEntry& Entry : FrameData.VisibleProxies)
		{
			if (!Entry.Proxy)
			{
				continue;
			}
			InitializeGlobalSortMetadata(
				GraphBuilder,
				View,
				Entry.Proxy,
				Entry.ProxySlot,
				Entry.SplatOffset,
				FrameData.GlobalMetadataPackedIndices,
				FrameData.GlobalMetadataDistances);
		}

		SortGlobalMetadata(
			GraphBuilder,
			FrameData.TotalVisibleSplats,
			FrameData.GlobalMetadataPackedIndices,
			FrameData.GlobalMetadataDistances);
	}

	for (const FVisibleProxyFrameEntry& Entry : FrameData.VisibleProxies)
	{
		FSplatSceneProxy* Proxy = Entry.Proxy;
		if (!Proxy)
		{
			continue;
		}

		uint32 NumSplats = Entry.NumSplats;

		FRDGPassRef ProjPass = ComputeTransforms(GraphBuilder, View, Proxy);
		if (bUseGlobalSort)
		{
			continue;
		}

		if (bIsSortingOnGPU)
		{
			FRDGBufferDesc IndexDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
			Proxy->GetIndicesFake() =
				GraphBuilder.CreateBuffer(IndexDesc, TEXT("Indices"));

			FRDGBufferDesc DistanceDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), NumSplats);
			Proxy->GetDistancesFake() =
				GraphBuilder.CreateBuffer(DistanceDesc, TEXT("Distances"));

			FRDGPassRef DistPass = CalculateDistances(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake());

			FRDGPassRef SortPass = SortSplats(
				GraphBuilder,
				View,
				Proxy,
				Proxy->GetIndicesFake(),
				Proxy->GetDistancesFake());
		}
		else
		{
			FRDGBufferDesc IndexDesc =
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
			Proxy->GetIndicesFake() = GraphBuilder.CreateBuffer(
				IndexDesc, TEXT("IndicesWithDistances"));

			Proxy->TryEnqueueSort(GetOrigin(View), GetForward(View));
		}
	}
}

void FSplatSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	if (IsPostTemporalCompositeEnabled_RenderThread())
	{
		return;
	}

	check(Inputs.SceneTextures);
	RenderSplats_RenderThread(
		GraphBuilder,
		View,
		(*Inputs.SceneTextures)->SceneColorTexture,
		(*Inputs.SceneTextures)->SceneDepthTexture);
}

void FSplatSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	(void)bIsPassEnabled;
	if (Pass == EPostProcessingPass::MotionBlur &&
	    IsPostTemporalCompositeEnabled_RenderThread())
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this,
			&FSplatSceneViewExtension::PostTemporalPass_RenderThread));
	}
}

FScreenPassTexture FSplatSceneViewExtension::PostTemporalPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	FScreenPassTexture SceneColor =
		Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	if (SceneColor.IsValid())
	{
		FRDGTextureRef SceneDepthTexture = nullptr;
		if (Inputs.SceneTextures.SceneTextures)
		{
			SceneDepthTexture = Inputs.SceneTextures.SceneTextures->GetParameters()
				->SceneDepthTexture;
		}
		RenderSplats_RenderThread(
			GraphBuilder,
			View,
			SceneColor.Texture,
			SceneDepthTexture);
	}
	return SceneColor;
}

void FSplatSceneViewExtension::RenderSplats_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture)
{
	SCOPE_CYCLE_COUNTER(STAT_PICOSplat_PrePostProcessPass);
	check(SceneColorTexture);
	TSharedPtr<FGlobalSortFrameData> PreparedFrameData =
		FindFrameData_RenderThread(View);
	if (PreparedFrameData)
	{
		INC_DWORD_STAT_BY(STAT_PICOSplat_VisibleProxiesPrePost,
			PreparedFrameData->VisibleProxies.Num());
	}
	if (PreparedFrameData && PreparedFrameData->bGlobalSortRequested &&
	    !PreparedFrameData->bGlobalSortSupported)
	{
		LogGlobalSortFallbackOnce_RenderThread();
		INC_DWORD_STAT(STAT_PICOSplat_GlobalSortFallbackFrames);
	}

	if (PreparedFrameData && PreparedFrameData->bGlobalSortRequested &&
	    PreparedFrameData->bGlobalSortSupported &&
	    PreparedFrameData->GlobalMetadataPackedIndices &&
	    PreparedFrameData->TotalVisibleSplats > 0)
	{
		FRDGTextureRef SplatTargetTexture = SceneColorTexture;
		ERenderTargetLoadAction SplatTargetLoadAction =
			ERenderTargetLoadAction::ELoad;
		const bool bUseCompositePipeline =
			IsCompositePipelineEnabled_RenderThread();
		if (bUseCompositePipeline)
		{
			FRDGTextureDesc SplatTargetDesc = SceneColorTexture->Desc;
			SplatTargetDesc.ClearValue = FClearValueBinding::Transparent;
			SplatTargetDesc.Flags |= TexCreate_RenderTargetable | TexCreate_ShaderResource;
			SplatTargetTexture = GraphBuilder.CreateTexture(
				SplatTargetDesc,
				TEXT("PICOSplat.CompositeTarget"));
			SplatTargetLoadAction = ERenderTargetLoadAction::EClear;
		}

		FGlobalSortFrameData& FrameData = *PreparedFrameData;
		const uint32 TotalVisibleSplats = FrameData.TotalVisibleSplats;
		INC_DWORD_STAT(STAT_PICOSplat_GlobalSortFrames);
		INC_DWORD_STAT_BY(STAT_PICOSplat_DrawableSplatsGlobal, TotalVisibleSplats);
		FRenderGlobalSplatDeps* PassParameters =
			GraphBuilder.AllocParameters<FRenderGlobalSplatDeps>();
		PassParameters->SortedIndices = GraphBuilder.CreateSRV(
			FrameData.GlobalMetadataPackedIndices,
			PF_R32_UINT);
		PassParameters->VS.SortedIndices = PassParameters->SortedIndices;
		PassParameters->VS.View = View.ViewUniformBuffer;
		PassParameters->VS.InstancedView =
			View.GetInstancedViewUniformBuffer();
		PassParameters->VS.SplatScale = GetRenderSplatScale_RenderThread();
		PassParameters->VS.bUseSphericalHarmonics = 0u;
		PassParameters->VS.SphericalHarmonicCoeffCount = 0u;
		PassParameters->PS.bSuperSplatCompatible =
			GetSuperSplatCompatibilityShaderFlag_RenderThread();
		PassParameters->PS.View = View.ViewUniformBuffer;
		PassParameters->PS.SplatAlphaGain = GetSplatAlphaGain_RenderThread();
		PassParameters->PS.SplatExposureIndependence =
			GetSplatExposureIndependence_RenderThread();

		const FVisibleProxyFrameEntry& DefaultEntry =
			FrameData.VisibleProxies[0];

		for (uint32 Slot = 0; Slot < Shaders::GLOBAL_RENDER_MAX_PROXIES; ++Slot)
		{
			PassParameters->VS.LocalToWorlds[Slot] = FMatrix44f::Identity;
			PassParameters->VS.PosMinsCM[Slot] = FVector4f::Zero();
			PassParameters->VS.PosScalesCM[Slot] = FVector4f::Zero();
			PassParameters->VS.ViewOriginsLocalCM[Slot] = FVector4f::Zero();
			SetGlobalProxyResources(
				PassParameters->VS,
				Slot,
				DefaultEntry.PositionsSRV.GetReference(),
				DefaultEntry.TransformsSRV.GetReference(),
				DefaultEntry.ColorsSRV.GetReference());
		}

		for (const FVisibleProxyFrameEntry& Entry : FrameData.VisibleProxies)
		{
			const uint32 Slot = Entry.ProxySlot;
			const FVector ViewOriginCM(GetOrigin(View));
			PassParameters->VS.LocalToWorlds[Slot] = Entry.LocalToWorld;
			PassParameters->VS.PosMinsCM[Slot] =
				FVector4f(Entry.PosMinCM, 0.f);
			PassParameters->VS.PosScalesCM[Slot] =
				FVector4f(Entry.PosScaleCM, 0.f);
			PassParameters->VS.ViewOriginsLocalCM[Slot] = FVector4f(
				FVector3f(Entry.Proxy->GetLocalToWorld().InverseTransformPosition(
					ViewOriginCM)),
				0.f);
			SetGlobalProxyResources(
				PassParameters->VS,
				Slot,
				Entry.PositionsSRV.GetReference(),
				Entry.TransformsSRV.GetReference(),
				Entry.ColorsSRV.GetReference());
		}

		PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(
			SplatTargetTexture,
			SplatTargetLoadAction);
		if (SceneDepthTexture)
		{
			PassParameters->PS.RenderTargets.DepthStencil = FDepthStencilBinding(
				SceneDepthTexture,
				ERenderTargetLoadAction::ELoad,
				FExclusiveDepthStencil::DepthWrite_StencilNop);
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME(
				"Splat: Render Global (%u)",
				TotalVisibleSplats),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, TotalVisibleSplats, &View](FRHICommandList& RHICmdList)
			{
				RenderGlobalSplats(
					RHICmdList,
					PassParameters,
					TotalVisibleSplats,
					View);
			});

		if (bUseCompositePipeline)
		{
			CompositeSplatTexture(
				GraphBuilder,
				View,
				SplatTargetTexture,
				SceneColorTexture);
		}
		return;
	}

	TSharedRef<FGlobalSortFrameData> FallbackFrameDataRef =
		BuildFrameData_RenderThread(View, true);
	FGlobalSortFrameData& FrameData = *FallbackFrameDataRef;
	if (FrameData.bGlobalSortRequested && !FrameData.bGlobalSortSupported)
	{
		LogGlobalSortFallbackOnce_RenderThread();
	}

	FRDGTextureRef SplatTargetTexture = SceneColorTexture;
	ERenderTargetLoadAction NextSplatTargetLoadAction =
		ERenderTargetLoadAction::ELoad;
	const bool bUseCompositePipeline =
		IsCompositePipelineEnabled_RenderThread() &&
		FrameData.VisibleProxies.Num() > 0;
	if (bUseCompositePipeline)
	{
		FRDGTextureDesc SplatTargetDesc = SceneColorTexture->Desc;
		SplatTargetDesc.ClearValue = FClearValueBinding::Transparent;
		SplatTargetDesc.Flags |= TexCreate_RenderTargetable | TexCreate_ShaderResource;
		SplatTargetTexture = GraphBuilder.CreateTexture(
			SplatTargetDesc,
			TEXT("PICOSplat.CompositeTarget"));
		NextSplatTargetLoadAction = ERenderTargetLoadAction::EClear;
	}

	for (const FVisibleProxyFrameEntry& Entry : FrameData.VisibleProxies)
	{
		FSplatSceneProxy* Proxy = Entry.Proxy;
		if (!Proxy)
		{
			continue;
		}

		INC_DWORD_STAT_BY(STAT_PICOSplat_DrawableSplatsLegacy,
			Proxy->GetNumDrawableSplats());

		if (!bIsSortingOnGPU)
		{
			FCPUSortRenderProducerParameters* SetupParameters =
				GraphBuilder
					.AllocParameters<FCPUSortRenderProducerParameters>();
			SetupParameters->IndicesUAV =
				GraphBuilder.CreateUAV(Proxy->GetIndicesFake(), PF_R32G32_UINT);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Splat: RDG Producer"),
				SetupParameters,
				ERDGPassFlags::Compute,
				[](FRHIComputeCommandList& RHICmdList) {});
		}

		Shaders::FRenderSplatSharedParameters Shared =
			SetSharedParameters(View, Proxy);
		Shaders::FRenderSplatPS::FParameters ParamsPS;
		ParamsPS.View = View.ViewUniformBuffer;
		ParamsPS.bSuperSplatCompatible =
			GetSuperSplatCompatibilityShaderFlag_RenderThread();
		ParamsPS.SplatAlphaGain = GetSplatAlphaGain_RenderThread();
		ParamsPS.SplatExposureIndependence =
			GetSplatExposureIndependence_RenderThread();
		ParamsPS.RenderTargets[0] = FRenderTargetBinding(
			SplatTargetTexture,
			NextSplatTargetLoadAction);
		if (SceneDepthTexture)
		{
			ParamsPS.RenderTargets.DepthStencil = FDepthStencilBinding(
				SceneDepthTexture,
				ERenderTargetLoadAction::ELoad,
				FExclusiveDepthStencil::DepthWrite_StencilNop);
		}

		if (bIsSortingOnGPU)
		{
			FRenderSplatGPUSortDeps* PassParameters =
				GraphBuilder.AllocParameters<FRenderSplatGPUSortDeps>();

			PassParameters->Indices =
				GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32_UINT);
			PassParameters->VS.Shared = Shared;
			PassParameters->VS.Indices = Proxy->GetIndicesSRV();
			PassParameters->PS = ParamsPS;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Splat: Render %s", *Proxy->GetName()),
				PassParameters,
				ERDGPassFlags::Raster,
				[this, PassParameters, Proxy, &View](
					FRHICommandList& RHICmdList)
				{
					RenderSplatGPUSort(
						RHICmdList,
						PassParameters,
						Proxy->GetNumSplats(),
						View);
				});
		}
		else
		{
			FRenderSplatCPUSortDeps* PassParameters =
				GraphBuilder.AllocParameters<FRenderSplatCPUSortDeps>();

			PassParameters->Indices =
				GraphBuilder.CreateSRV(Proxy->GetIndicesFake(), PF_R32G32_UINT);
			PassParameters->VS.Shared = Shared;
			PassParameters->VS.Indices =
				Proxy->GetIndicesSRV(); // (Index, Distance).
			PassParameters->PS = ParamsPS;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Splat: Render %s", *Proxy->GetName()),
				PassParameters,
				ERDGPassFlags::Raster,
				[this, PassParameters, Proxy, &View](
					FRHICommandList& RHICmdList)
				{
					RenderSplatCPUSort(
						RHICmdList,
						PassParameters,
						Proxy->GetNumDrawableSplats(),
						View);
				});
		}

		if (bUseCompositePipeline)
		{
			NextSplatTargetLoadAction = ERenderTargetLoadAction::ELoad;
		}
	}

	if (bUseCompositePipeline)
	{
		CompositeSplatTexture(
			GraphBuilder,
			View,
			SplatTargetTexture,
			SceneColorTexture);
	}
}

void FSplatSceneViewExtension::PostRenderBasePassMobile_RenderThread(
	FRHICommandList& RHICmdList, FSceneView& InView)
{
	TSharedRef<FGlobalSortFrameData> MobileFrameDataRef =
		BuildFrameData_RenderThread(InView, true);
	FGlobalSortFrameData& FrameData = *MobileFrameDataRef;
	if (FrameData.bGlobalSortRequested && !FrameData.bGlobalSortSupported)
	{
		LogGlobalSortFallbackOnce_RenderThread();
	}

	for (const FVisibleProxyFrameEntry& Entry : FrameData.VisibleProxies)
	{
		FSplatSceneProxy* Proxy = Entry.Proxy;
		if (!Proxy)
		{
			continue;
		}

		Shaders::FRenderSplatSharedParameters Shared =
			SetSharedParameters(InView, Proxy);

		SCOPED_DRAW_EVENTF(
			RHICmdList,
			RenderSplat,
			TEXT("Splat: Render %s"),
			*Proxy->GetName());
		if (bIsSortingOnGPU)
		{
			FRenderSplatGPUSortDeps Parameters{};
			Parameters.VS.Shared = Shared;
			Parameters.VS.Indices = Proxy->GetIndicesSRV();
			Parameters.PS.View = InView.ViewUniformBuffer;
			Parameters.PS.bSuperSplatCompatible =
				GetSuperSplatCompatibilityShaderFlag_RenderThread();
			Parameters.PS.SplatAlphaGain = GetSplatAlphaGain_RenderThread();
			Parameters.PS.SplatExposureIndependence =
				GetSplatExposureIndependence_RenderThread();
			RenderSplatGPUSort(
				RHICmdList, &Parameters, Proxy->GetNumSplats(), InView);
		}
		else
		{
			FRenderSplatCPUSortDeps Parameters{};
			Parameters.VS.Shared = Shared;
			Parameters.VS.Indices = Proxy->GetIndicesSRV();
			Parameters.PS.View = InView.ViewUniformBuffer;
			Parameters.PS.bSuperSplatCompatible =
				GetSuperSplatCompatibilityShaderFlag_RenderThread();
			Parameters.PS.SplatAlphaGain = GetSplatAlphaGain_RenderThread();
			Parameters.PS.SplatExposureIndependence =
				GetSplatExposureIndependence_RenderThread();
			RenderSplatCPUSort(
				RHICmdList, &Parameters, Proxy->GetNumDrawableSplats(), InView);
		}
	}
}

#undef PICOSPLAT_ASSIGN_GLOBAL_PROXY_RESOURCES

} // namespace PICO::Splat