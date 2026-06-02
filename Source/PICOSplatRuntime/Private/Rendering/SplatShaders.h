/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include <type_traits>

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HLSLTypeAliases.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

namespace PICO::Splat::Shaders
{

#define PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(Suffix) \
	SHADER_PARAMETER_SRV(Buffer<uint2>, Positions##Suffix) \
	SHADER_PARAMETER_SRV(Buffer<float4>, Transforms##Suffix) \
	SHADER_PARAMETER_SRV(Buffer<float4>, Colors##Suffix)

/**
 * For compute shader pre-passes only.
 * TODO(seth): This needs to be tuned for performance.
 */
constexpr uint32 THREAD_GROUP_SIZE_X = 32;
constexpr uint32 GLOBAL_SORT_PROXY_SLOT_BITS = 8;
constexpr uint32 GLOBAL_SORT_LOCAL_INDEX_BITS =
	32 - GLOBAL_SORT_PROXY_SLOT_BITS;
constexpr uint32 GLOBAL_SORT_PROXY_SLOT_SHIFT =
	GLOBAL_SORT_LOCAL_INDEX_BITS;
constexpr uint32 GLOBAL_SORT_MAX_PROXIES =
	1u << GLOBAL_SORT_PROXY_SLOT_BITS;
constexpr uint32 GLOBAL_SORT_MAX_LOCAL_INDEX =
	(1u << GLOBAL_SORT_LOCAL_INDEX_BITS) - 1u;
/**
 * Maximum number of distinct splat proxies that can be batched into a single
 * global render pass.
 *
 * Tied to the fixed-size HLSL uniform arrays in RenderGlobalSplatVS.usf and
 * the explicit slot switch in `SetGlobalProxyResources`. Raising this requires:
 *   1. Updating the HLSL `LocalToWorlds`/`PosMinsCM`/`PosScalesCM` arrays.
 *   2. Updating the C++ `LocalToWorlds`/... C-arrays in this file.
 *   3. Adding extra `case` entries to `SetGlobalProxyResources`.
 *   4. Verifying the platform shader resource binding count. The global path
 *      intentionally does not bind per-proxy SH buffers: each proxy uses 3
 *      SRVs and the sorted index buffer uses 1 SRV. D3D SM6 supports 64 SRVs,
 *      so the safe compile-time cap is 21 proxies.
 *
 * To support arbitrarily many proxies without a shader change, the engine
 * would need to switch to chunked multi-pass rendering (back-to-front per
 * chunk), which preserves intra-chunk sort fidelity but loses cross-chunk
 * splat interleaving. That work is tracked separately.
 */
constexpr uint32 GLOBAL_RENDER_MAX_PROXIES = 21;

BEGIN_SHADER_PARAMETER_STRUCT(FPackedPositionParameters, )
SHADER_PARAMETER(FVector3f, pos_min_cm)
SHADER_PARAMETER(FVector3f, pos_scale_cm)
SHADER_PARAMETER_SRV(Buffer<uint2>, positions)
END_SHADER_PARAMETER_STRUCT()

/**
 * Calculates distances to each splat, for GPU sorting.
 */
class FComputeDistanceCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeDistanceCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeDistanceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_clip)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_UAV(RWBuffer<uint>, indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, distances)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * Calculates 2x2 transform for each splat.
 */
class FComputeTransformCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeTransformCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeTransformCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_view)
	SHADER_PARAMETER(float, two_focal_length)
	SHADER_PARAMETER(float, min_screen_variance_px)
	SHADER_PARAMETER(float, cov_scale_cm2)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_SRV(Buffer<uint2>, covariances)
	SHADER_PARAMETER_UAV(RWBuffer<float4>, transforms)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
	}
};

/**
 * Populates frame-global metadata buffers for the experimental cross-actor
 * sorting path. Each dispatch writes one proxy slice worth of
 * (proxy_slot, local_splat_index, distance).
 */
class FInitializeGlobalSortMetadataCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FInitializeGlobalSortMetadataCS);
	SHADER_USE_PARAMETER_STRUCT(FInitializeGlobalSortMetadataCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, local_to_clip)
	SHADER_PARAMETER(uint32, proxy_slot)
	SHADER_PARAMETER(uint32, global_offset)
	SHADER_PARAMETER(uint32, num_splats)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, metadata_indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, metadata_distances)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREAD_GROUP_SIZE_X"), THREAD_GROUP_SIZE_X);
		OutEnvironment.SetDefine(
			TEXT("GLOBAL_SORT_PROXY_SLOT_SHIFT"),
			GLOBAL_SORT_PROXY_SLOT_SHIFT);
	}
};

/**
 * For controlling shader parameters in RenderSplatVS.
 */
enum class ESortingDevice : uint8
{
	GPU = 0,
	CPU = 1
};

/**
 * Parameters shared between both CPU and GPU sorting versions.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderSplatSharedParameters, )
SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
SHADER_PARAMETER_STRUCT_REF(
	FInstancedViewUniformShaderParameters, InstancedView)
SHADER_PARAMETER(FMatrix44f, local_to_world)
SHADER_PARAMETER(FVector3f, view_origin_local_cm)
SHADER_PARAMETER(float, splat_scale)
SHADER_PARAMETER(uint32, bUseSphericalHarmonics)
SHADER_PARAMETER(uint32, spherical_harmonic_coeff_count)
SHADER_PARAMETER_STRUCT_INCLUDE(FPackedPositionParameters, Positions)
SHADER_PARAMETER_SRV(Buffer<float4>, transforms)
SHADER_PARAMETER_SRV(Buffer<float4>, colors)
SHADER_PARAMETER_SRV(Buffer<float4>, spherical_harmonics)
END_SHADER_PARAMETER_STRUCT()

/**
 * Per splat, creates a containing triangle.
 */
template <ESortingDevice Device>
class FRenderSplatVS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FRenderSplatVS, FGlobalShader);

	// With HLSLTypeAliases.h, gives access to HLSL style types outside parameter
	// struct.
	using T = std::conditional_t<
		Device == ESortingDevice::GPU,
		UE::HLSL::uint,
		UE::HLSL::uint2>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FRenderSplatSharedParameters, Shared)
	SHADER_PARAMETER_SRV(Buffer<T>, Indices)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		if constexpr (Device == ESortingDevice::GPU)
		{
			FGlobalShader::ModifyCompilationEnvironment(
				Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("GPU_SORT"), 1);
		}
	}
};

/**
 * Global draw path that resolves sorted splats across multiple proxies.
 */
class FRenderGlobalSplatVS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderGlobalSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FRenderGlobalSplatVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_REF(
		FInstancedViewUniformShaderParameters, InstancedView)
	SHADER_PARAMETER_ARRAY(
		FMatrix44f,
		LocalToWorlds,
		[GLOBAL_RENDER_MAX_PROXIES])
	SHADER_PARAMETER_ARRAY(
		FVector4f,
		PosMinsCM,
		[GLOBAL_RENDER_MAX_PROXIES])
	SHADER_PARAMETER_ARRAY(
		FVector4f,
		PosScalesCM,
		[GLOBAL_RENDER_MAX_PROXIES])
	SHADER_PARAMETER_ARRAY(
		FVector4f,
		ViewOriginsLocalCM,
		[GLOBAL_RENDER_MAX_PROXIES])
	SHADER_PARAMETER(float, SplatScale)
	SHADER_PARAMETER(uint32, bUseSphericalHarmonics)
	SHADER_PARAMETER(uint32, SphericalHarmonicCoeffCount)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedIndices)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(0)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(1)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(2)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(3)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(4)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(5)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(6)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(7)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(8)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(9)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(10)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(11)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(12)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(13)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(14)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(15)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(16)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(17)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(18)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(19)
	PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES(20)
	END_SHADER_PARAMETER_STRUCT()

public:
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("GLOBAL_SORT_PROXY_SLOT_SHIFT"),
			GLOBAL_SORT_PROXY_SLOT_SHIFT);
		OutEnvironment.SetDefine(
			TEXT("GLOBAL_SORT_LOCAL_INDEX_MASK"),
			GLOBAL_SORT_MAX_LOCAL_INDEX);
		OutEnvironment.SetDefine(
			TEXT("GLOBAL_RENDER_MAX_PROXIES"),
			GLOBAL_RENDER_MAX_PROXIES);
	}
};

#undef PICOSPLAT_DECLARE_GLOBAL_PROXY_RESOURCES

/**
 * Draws a splat into each triangle.
 */
class FRenderSplatPS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FRenderSplatPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(uint32, bSuperSplatCompatible)
	SHADER_PARAMETER(float, SplatAlphaGain)
	SHADER_PARAMETER(float, SplatExposureIndependence)
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * Composites the dedicated splat render target back into SceneColor.
 */
class FCompositeSplatPS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompositeSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FCompositeSplatPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SplatTexture)
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

} // namespace PICO::Splat::Shaders