/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatRendering.h"

#include "GPUSort.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AssertionMacros.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "SplatConstants.h"
#include "SplatRenderingUtilities.h"

namespace PICO::Splat
{
namespace
{
/**
 * HACK(seth): I'm lying to the RDG using fake SRVs to track resources not
 * actually managed by the RDG. As such, I have to pretend to write to the
 * resource in order to pass validation.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortProducerParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndicesUAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Indices2UAV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Distances2UAV)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortParameters, )
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndicesSRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndicesUAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Indices2SRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Indices2UAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DistancesSRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DistancesUAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Distances2SRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Distances2UAV)
END_SHADER_PARAMETER_STRUCT()

uint32 NumThreadGroups(uint32 NumElements)
{
	return (NumElements + (Shaders::THREAD_GROUP_SIZE_X - 1)) /
	       Shaders::THREAD_GROUP_SIZE_X;
}

TAutoConsoleVariable<int32> CVarPICOSplatSuperSplatCompatibility(
	TEXT("r.PICOSplat.SuperSplatCompatibility"),
	1,
	TEXT("Enable SuperSplat/PlayCanvas-style splat raster compatibility.\n")
	TEXT("0: Disabled, uses the original PICOSplat falloff and straight-alpha blend.\n")
	TEXT("1: Enabled (default), uses normalized cutoff falloff, premultiplied alpha, and the configured minimum screen variance."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarPICOSplatSuperSplatMinScreenVariance(
	TEXT("r.PICOSplat.SuperSplatMinScreenVariance"),
	0.3f,
	TEXT("Minimum screen-space variance, in pixels squared, added to each splat axis when SuperSplat compatibility is enabled. ")
	TEXT("The 3DGS paper and common web viewers use values around 0.3 to reduce holes and aliasing."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPICOSplatCompositePipeline(
	TEXT("r.PICOSplat.CompositePipeline"),
	1,
	TEXT("Render splats into a dedicated transparent render target before compositing into SceneColor.\n")
	TEXT("0: Disabled, draw splats directly into SceneColor.\n")
	TEXT("1: Enabled (default), closer to SuperSplat's dedicated splat layer/composite path."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarPICOSplatAlphaGain(
	TEXT("r.PICOSplat.AlphaGain"),
	1.35f,
	TEXT("Opacity gain applied to each splat fragment without changing splat size. ")
	TEXT("Uses 1 - pow(1 - alpha, gain), so 1.0 preserves imported alpha; ")
	TEXT("values around 1.2-2.0 can reduce see-through gaps on bright backgrounds."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarPICOSplatExposureIndependence(
	TEXT("r.PICOSplat.ExposureIndependence"),
	1.0f,
	TEXT("How much splat color should ignore UE eye adaptation/exposure.\n")
	TEXT("0: UE-native pre-exposed scene color, affected by auto exposure.\n")
	TEXT("1: Exposure-independent splat color (default), reducing brightness/opacity drift when r.EyeAdaptationQuality changes."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPICOSplatSphericalHarmonics(
	TEXT("r.PICOSplat.SphericalHarmonics"),
	1,
	TEXT("Evaluate imported f_rest_* spherical harmonic color coefficients when present.\n")
	TEXT("0: Disabled\n")
	TEXT("1: Enabled (default). Requires re-imported assets with SH data."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarPICOSplatSphericalHarmonicBands(
	TEXT("r.PICOSplat.SphericalHarmonicBands"),
	3,
	TEXT("Maximum non-DC spherical harmonic bands to evaluate. Clamped to 0-3; default 3."),
	ECVF_RenderThreadSafe);

FBlendStateRHIRef GetRenderSplatBlendState_RenderThread()
{
	if (IsSuperSplatCompatibilityEnabled_RenderThread())
	{
		return TStaticBlendState<
			CW_RGBA,
			BO_Add,
			BF_One,
			BF_InverseSourceAlpha,
			BO_Add,
			BF_One,
			BF_InverseSourceAlpha>::GetRHI();
	}

	return TStaticBlendState<
		CW_RGBA,
		BO_Add,
		BF_SourceAlpha,
		BF_InverseSourceAlpha>::GetRHI();
}
} // namespace

bool IsSuperSplatCompatibilityEnabled_RenderThread()
{
	return CVarPICOSplatSuperSplatCompatibility.GetValueOnRenderThread() != 0;
}

float GetSuperSplatMinScreenVariance_RenderThread()
{
	if (!IsSuperSplatCompatibilityEnabled_RenderThread())
	{
		return 0.0f;
	}

	return FMath::Max(
		0.0f,
		CVarPICOSplatSuperSplatMinScreenVariance.GetValueOnRenderThread());
}

uint32 GetSuperSplatCompatibilityShaderFlag_RenderThread()
{
	return IsSuperSplatCompatibilityEnabled_RenderThread() ? 1u : 0u;
}

bool IsCompositePipelineEnabled_RenderThread()
{
	return CVarPICOSplatCompositePipeline.GetValueOnRenderThread() != 0;
}

float GetSplatAlphaGain_RenderThread()
{
	return FMath::Max(0.01f, CVarPICOSplatAlphaGain.GetValueOnRenderThread());
}

float GetSplatExposureIndependence_RenderThread()
{
	return FMath::Clamp(
		CVarPICOSplatExposureIndependence.GetValueOnRenderThread(), 0.0f, 1.0f);
}

uint32 GetSphericalHarmonicsShaderFlag_RenderThread()
{
	return CVarPICOSplatSphericalHarmonics.GetValueOnRenderThread() != 0 ? 1u : 0u;
}

uint32 GetSphericalHarmonicsCoeffCount_RenderThread()
{
	if (GetSphericalHarmonicsShaderFlag_RenderThread() == 0)
	{
		return 0u;
	}

	switch (FMath::Clamp(
		CVarPICOSplatSphericalHarmonicBands.GetValueOnRenderThread(), 0, 3))
	{
	case 1:
		return 3u;
	case 2:
		return 8u;
	case 3:
		return 15u;
	default:
		return 0u;
	}
}

FRDGPassRef CalculateDistances(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances)
{
	check(Proxy);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FComputeDistanceCS> DistanceShader =
		GlobalShaderMap->GetShader<Shaders::FComputeDistanceCS>();

	FRDGBufferUAV* IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	FRDGBufferUAV* DistancesUAV =
		GraphBuilder.CreateUAV(Distances, PF_R16_UINT);

	Shaders::FComputeDistanceCS::FParameters* DistanceParams =
		GraphBuilder
			.AllocParameters<Shaders::FComputeDistanceCS::FParameters>();
	DistanceParams->local_to_clip =
		FMatrix44f(Proxy->GetLocalToWorld() * GetViewProj(View));
	DistanceParams->num_splats = Proxy->GetNumSplats();
	DistanceParams->Positions = MakePositionParams(Proxy);
	DistanceParams->indices = Proxy->GetIndicesUAV();
	DistanceParams->distances = DistancesUAV;

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Distances %s", *Proxy->GetResourceName().ToString()),
		ERDGPassFlags::AsyncCompute,
		DistanceShader,
		DistanceParams,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

FRDGPassRef ComputeTransforms(
	FRDGBuilder& GraphBuilder, const FSceneView& View, FSplatSceneProxy* Proxy)
{
	check(Proxy);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FComputeTransformCS> ComputeSplatTransforms =
		GlobalShaderMap->GetShader<Shaders::FComputeTransformCS>();

	Shaders::FComputeTransformCS::FParameters* SplatParams =
		GraphBuilder
			.AllocParameters<Shaders::FComputeTransformCS::FParameters>();
	SplatParams->local_to_view =
		FMatrix44f(Proxy->GetLocalToWorld() * GetView(View));
	SplatParams->two_focal_length = 2 * GetFocalLength(View);
	SplatParams->min_screen_variance_px =
		GetSuperSplatMinScreenVariance_RenderThread();
	SplatParams->cov_scale_cm2 = Proxy->GetCovarianceScaleCM2();
	SplatParams->num_splats = Proxy->GetNumSplats();
	SplatParams->Positions = MakePositionParams(Proxy);
	SplatParams->covariances = Proxy->GetCovariancesSRV();
	SplatParams->transforms = Proxy->GetTransformsUAV();

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Transforms %s", *Proxy->GetResourceName().ToString()),
		ERDGPassFlags::AsyncCompute,
		ComputeSplatTransforms,
		SplatParams,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

FRDGPassRef InitializeGlobalSortMetadata(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	uint32 ProxySlot,
	uint32 GlobalOffset,
	FRDGBufferRef MetadataPackedIndices,
	FRDGBufferRef MetadataDistances)
{
	check(Proxy);
	check(MetadataPackedIndices);
	check(MetadataDistances);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FInitializeGlobalSortMetadataCS> InitializeShader =
		GlobalShaderMap->GetShader<Shaders::FInitializeGlobalSortMetadataCS>();

	Shaders::FInitializeGlobalSortMetadataCS::FParameters* Parameters =
		GraphBuilder.AllocParameters<
			Shaders::FInitializeGlobalSortMetadataCS::FParameters>();
	Parameters->local_to_clip =
		FMatrix44f(Proxy->GetLocalToWorld() * GetViewProj(View));
	Parameters->proxy_slot = ProxySlot;
	Parameters->global_offset = GlobalOffset;
	Parameters->num_splats = Proxy->GetNumSplats();
	Parameters->Positions = MakePositionParams(Proxy);
	Parameters->metadata_indices =
		GraphBuilder.CreateUAV(MetadataPackedIndices, PF_R32_UINT);
	Parameters->metadata_distances =
		GraphBuilder.CreateUAV(MetadataDistances, PF_R16_UINT);

	return FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME(
			"Splat: Init Global Metadata Proxy=%u Offset=%u Count=%u",
			ProxySlot,
			GlobalOffset,
			Proxy->GetNumSplats()),
		ERDGPassFlags::AsyncCompute,
		InitializeShader,
		Parameters,
		FIntVector(NumThreadGroups(Proxy->GetNumSplats()), 1, 1));
}

FRDGPassRef SortGlobalMetadata(
	FRDGBuilder& GraphBuilder,
	uint32 NumSplats,
	FRDGBufferRef MetadataPackedIndices,
	FRDGBufferRef MetadataDistances)
{
	check(MetadataPackedIndices);
	check(MetadataDistances);

	FRDGBufferDesc IndexDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
	FRDGBuffer* Indices2 =
		GraphBuilder.CreateBuffer(IndexDesc, TEXT("GlobalMetadataIndices2"));

	FRDGBufferDesc DistanceDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), NumSplats);
	FRDGBuffer* Distances2 = GraphBuilder.CreateBuffer(
		DistanceDesc, TEXT("GlobalMetadataDistances2"));

	FGPUSortProducerParameters* SetupParameters =
		GraphBuilder.AllocParameters<FGPUSortProducerParameters>();
	SetupParameters->IndicesUAV =
		GraphBuilder.CreateUAV(MetadataPackedIndices, PF_R32_UINT);
	SetupParameters->Indices2UAV = GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SetupParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: RDG Producer Global Metadata"),
		SetupParameters,
		ERDGPassFlags::Compute,
		[](FRHIComputeCommandList& RHICmdList) {});

	FGPUSortParameters* SortParameters =
		GraphBuilder.AllocParameters<FGPUSortParameters>();
	SortParameters->IndicesSRV =
		GraphBuilder.CreateSRV(MetadataPackedIndices, PF_R32_UINT);
	SortParameters->IndicesUAV =
		GraphBuilder.CreateUAV(MetadataPackedIndices, PF_R32_UINT);
	SortParameters->Indices2SRV = GraphBuilder.CreateSRV(Indices2, PF_R32_UINT);
	SortParameters->Indices2UAV = GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SortParameters->DistancesSRV =
		GraphBuilder.CreateSRV(MetadataDistances, PF_R16_UINT);
	SortParameters->DistancesUAV =
		GraphBuilder.CreateUAV(MetadataDistances, PF_R16_UINT);
	SortParameters->Distances2SRV =
		GraphBuilder.CreateSRV(Distances2, PF_R16_UINT);
	SortParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	return GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: Sort Global Metadata (%u)", NumSplats),
		SortParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[NumSplats, SortParameters](FRHIComputeCommandList& RHICmdList)
		{
			FGPUSortBuffers SortBuffers;
			SortBuffers.RemoteKeySRVs[0] =
				SortParameters->DistancesSRV->GetRHI();
			SortBuffers.RemoteKeySRVs[1] =
				SortParameters->Distances2SRV->GetRHI();
			SortBuffers.RemoteKeyUAVs[0] =
				SortParameters->DistancesUAV->GetRHI();
			SortBuffers.RemoteKeyUAVs[1] =
				SortParameters->Distances2UAV->GetRHI();
			SortBuffers.RemoteValueSRVs[0] =
				SortParameters->IndicesSRV->GetRHI();
			SortBuffers.RemoteValueSRVs[1] =
				SortParameters->Indices2SRV->GetRHI();
			SortBuffers.RemoteValueUAVs[0] =
				SortParameters->IndicesUAV->GetRHI();
			SortBuffers.RemoteValueUAVs[1] =
				SortParameters->Indices2UAV->GetRHI();

			int32 ResultIndex = SortGPUBuffers(
				static_cast<FRHICommandList&>(RHICmdList),
				SortBuffers,
				0,
				DepthMask,
				NumSplats,
				GMaxRHIFeatureLevel);
			check(ResultIndex == 0);
		});
}

void RenderSplatCPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatCPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View)
{
	check(SplatParameters);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FRenderSplatVS<Shaders::ESortingDevice::CPU>>
		VertexShader = GlobalShaderMap->GetShader<
			Shaders::FRenderSplatVS<Shaders::ESortingDevice::CPU>>();
	TShaderRef<Shaders::FRenderSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FRenderSplatPS>();

	/**
	 * Sometimes in editor, the displayed area is smaller than the actual
	 * viewport size. By shrinking the viewport to the correct size, we avoid
	 * rendering the splats incorrectly (as they rely on knowing the viewport
	 * size for projection).
	 */
	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	RHICmdList.SetViewport(
		float(ViewRect.Min.X),
		float(ViewRect.Min.Y),
		0.f,
		float(ViewRect.Max.X),
		float(ViewRect.Max.Y),
		1.f);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
		PipelineStateCache::GetOrCreateVertexDeclaration({});
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI =
		VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
		PixelShader.GetPixelShader();
	GraphicsPSOInit.DepthStencilState =
		TStaticDepthStencilState<false>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.BlendState = GetRenderSplatBlendState_RenderThread();
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	SetShaderParameters(
		RHICmdList,
		VertexShader,
		VertexShader.GetVertexShader(),
		SplatParameters->VS);
	SetShaderParameters(
		RHICmdList,
		PixelShader,
		PixelShader.GetPixelShader(),
		SplatParameters->PS);

	RHICmdList.DrawPrimitive(0, 2 * NumSplats, 1);
}

void RenderSplatGPUSort(
	FRHICommandList& RHICmdList,
	FRenderSplatGPUSortDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View)
{
	check(SplatParameters);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FRenderSplatVS<Shaders::ESortingDevice::GPU>>
		VertexShader = GlobalShaderMap->GetShader<
			Shaders::FRenderSplatVS<Shaders::ESortingDevice::GPU>>();
	TShaderRef<Shaders::FRenderSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FRenderSplatPS>();

	/**
	 * Sometimes in editor, the displayed area is smaller than the actual
	 * viewport size. By shrinking the viewport to the correct size, we avoid
	 * rendering the splats incorrectly (as they rely on knowing the viewport
	 * size for projection).
	 */
	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	RHICmdList.SetViewport(
		float(ViewRect.Min.X),
		float(ViewRect.Min.Y),
		0.f,
		float(ViewRect.Max.X),
		float(ViewRect.Max.Y),
		1.f);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
		PipelineStateCache::GetOrCreateVertexDeclaration({});
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI =
		VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
		PixelShader.GetPixelShader();
	GraphicsPSOInit.DepthStencilState =
		TStaticDepthStencilState<false>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.BlendState = GetRenderSplatBlendState_RenderThread();
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	SetShaderParameters(
		RHICmdList,
		VertexShader,
		VertexShader.GetVertexShader(),
		SplatParameters->VS);
	SetShaderParameters(
		RHICmdList,
		PixelShader,
		PixelShader.GetPixelShader(),
		SplatParameters->PS);

	RHICmdList.DrawPrimitive(0, 2 * NumSplats, 1);
}

void RenderGlobalSplats(
	FRHICommandList& RHICmdList,
	FRenderGlobalSplatDeps* SplatParameters,
	uint32 NumSplats,
	const FSceneView& View)
{
	check(SplatParameters);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FRenderGlobalSplatVS> VertexShader =
		GlobalShaderMap->GetShader<Shaders::FRenderGlobalSplatVS>();
	TShaderRef<Shaders::FRenderSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FRenderSplatPS>();

	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	RHICmdList.SetViewport(
		float(ViewRect.Min.X),
		float(ViewRect.Min.Y),
		0.f,
		float(ViewRect.Max.X),
		float(ViewRect.Max.Y),
		1.f);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
		PipelineStateCache::GetOrCreateVertexDeclaration({});
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI =
		VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI =
		PixelShader.GetPixelShader();
	GraphicsPSOInit.DepthStencilState =
		TStaticDepthStencilState<false>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.BlendState = GetRenderSplatBlendState_RenderThread();
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	Shaders::FRenderGlobalSplatVS::FParameters VertexParameters =
		SplatParameters->VS;
	VertexParameters.SortedIndices = SplatParameters->SortedIndices;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
	SetShaderParameters(
		RHICmdList,
		VertexShader,
		VertexShader.GetVertexShader(),
		VertexParameters);
	SetShaderParameters(
		RHICmdList,
		PixelShader,
		PixelShader.GetPixelShader(),
		SplatParameters->PS);

	RHICmdList.DrawPrimitive(0, 2 * NumSplats, 1);
}

void CompositeSplatTexture(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SplatTexture,
	FRDGTextureRef SceneColorTexture)
{
	check(SplatTexture);
	check(SceneColorTexture);

	const FGlobalShaderMap* GlobalShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderRef<Shaders::FCompositeSplatPS> PixelShader =
		GlobalShaderMap->GetShader<Shaders::FCompositeSplatPS>();

	Shaders::FCompositeSplatPS::FParameters* Parameters =
		GraphBuilder.AllocParameters<Shaders::FCompositeSplatPS::FParameters>();
	Parameters->SplatTexture = SplatTexture;
	Parameters->RenderTargets[0] = FRenderTargetBinding(
		SceneColorTexture,
		ERenderTargetLoadAction::ELoad);

	check(View.bIsViewInfo);
	const FIntRect ViewRect = static_cast<const FViewInfo&>(View).ViewRect;
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GlobalShaderMap,
		RDG_EVENT_NAME("Splat: Composite"),
		PixelShader,
		Parameters,
		ViewRect,
		TStaticBlendState<
			CW_RGBA,
			BO_Add,
			BF_One,
			BF_InverseSourceAlpha,
			BO_Add,
			BF_One,
			BF_InverseSourceAlpha>::GetRHI());
}

FRDGPassRef SortSplats(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FSplatSceneProxy* Proxy,
	FRDGBufferRef Indices,
	FRDGBufferRef Distances)
{
	check(Proxy);
	check(Indices);
	check(Distances);

	uint32 NumSplats = Proxy->GetNumSplats();

	FRDGBufferDesc IndexDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats);
	FRDGBuffer* Indices2 =
		GraphBuilder.CreateBuffer(IndexDesc, TEXT("Indices2"));

	FRDGBufferDesc DistanceDesc =
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), NumSplats);
	FRDGBuffer* Distances2 =
		GraphBuilder.CreateBuffer(DistanceDesc, TEXT("Distances2"));

	FGPUSortProducerParameters* SetupParameters =
		GraphBuilder.AllocParameters<FGPUSortProducerParameters>();
	SetupParameters->IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	SetupParameters->Indices2UAV =
		GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SetupParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: RDG Producer"),
		SetupParameters,
		ERDGPassFlags::Compute,
		[](FRHIComputeCommandList& RHICmdList) {});

	FGPUSortParameters* SortParameters =
		GraphBuilder.AllocParameters<FGPUSortParameters>();
	SortParameters->IndicesSRV = GraphBuilder.CreateSRV(Indices, PF_R32_UINT);
	SortParameters->IndicesUAV = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
	SortParameters->Indices2SRV = GraphBuilder.CreateSRV(Indices2, PF_R32_UINT);
	SortParameters->Indices2UAV = GraphBuilder.CreateUAV(Indices2, PF_R32_UINT);
	SortParameters->DistancesSRV =
		GraphBuilder.CreateSRV(Distances, PF_R16_UINT);
	SortParameters->DistancesUAV =
		GraphBuilder.CreateUAV(Distances, PF_R16_UINT);
	SortParameters->Distances2SRV =
		GraphBuilder.CreateSRV(Distances2, PF_R16_UINT);
	SortParameters->Distances2UAV =
		GraphBuilder.CreateUAV(Distances2, PF_R16_UINT);

	// `Compute` used for mobile support, but this could be `AsyncCompute`.
	// `NeverCull` ensures that this pass still happens even if RDG doesn't think
	// resources are being used.
	return GraphBuilder.AddPass(
		RDG_EVENT_NAME("Splat: Sort %s", *Proxy->GetName()),
		SortParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[NumSplats,
	     SortParameters,
	     SRV = Proxy->GetIndicesSRV(),
	     UAV = Proxy->GetIndicesUAV()](FRHIComputeCommandList& RHICmdList)
		{
			FGPUSortBuffers SortBuffers;
			SortBuffers.RemoteKeySRVs[0] =
				SortParameters->DistancesSRV->GetRHI();
			SortBuffers.RemoteKeySRVs[1] =
				SortParameters->Distances2SRV->GetRHI();
			SortBuffers.RemoteKeyUAVs[0] =
				SortParameters->DistancesUAV->GetRHI();
			SortBuffers.RemoteKeyUAVs[1] =
				SortParameters->Distances2UAV->GetRHI();
			SortBuffers.RemoteValueSRVs[0] = SRV;
			SortBuffers.RemoteValueSRVs[1] =
				SortParameters->Indices2SRV->GetRHI();
			SortBuffers.RemoteValueUAVs[0] = UAV;
			SortBuffers.RemoteValueUAVs[1] =
				SortParameters->Indices2UAV->GetRHI();

			int32 ResultIndex = SortGPUBuffers(
				static_cast<FRHICommandList&>(RHICmdList),
				SortBuffers,
				0,
				DepthMask,
				NumSplats,
				GMaxRHIFeatureLevel);
			check(ResultIndex == 0);
		});
}
} // namespace PICO::Splat