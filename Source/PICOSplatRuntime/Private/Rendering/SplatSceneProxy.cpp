/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "SplatSceneProxy.h"

#include "MaterialDomain.h"
#include "Materials/MaterialRenderProxy.h"
#include "PackedTypes.h"
#include "SplatConstants.h"
#include "SplatSettings.h"
#include "SplatSubsystem.h"

#if WITH_EDITOR
#include "Materials/Material.h"
#include "SceneManagement.h"
#endif

namespace PICO::Splat
{
#if WITH_EDITOR
namespace
{
FLinearColor MakeHullDebugColor(int32 HullIndex)
{
	const uint8 Hue = static_cast<uint8>((HullIndex * 53) % 255);
	return FLinearColor::MakeFromHSV8(Hue, 160, 255);
}
} // namespace
#endif

FSplatSceneProxy::FSplatSceneProxy(USplatComponent& Component)
	: FPrimitiveSceneProxy(&Component)
	, Asset(Component.GetAsset())
	, NumSplatsCached(Component.GetAsset()->GetNumSplats())
	, Transforms(Component.GetAsset()->GetNumSplats(), EPixelFormat::PF_FloatRGBA)
	, bIsSortingOnGPU(USplatSettings::IsSortingOnGPU())
#if WITH_EDITOR
	, bForceSingleConvexHull(Component.GetForceSingleConvexHull())
	, VertexFactory(GetScene().GetFeatureLevel(), "FSplatSceneProxy")
	, BodySetup(Component.GetBodySetup())
#endif
{
	USplatAsset* AssetPtr = Component.GetAsset();
	check(AssetPtr);

	// Cache RHI references and packed-position constants up front so the
	// render thread no longer needs to dereference the UObject. RHI refs are
	// independently refcounted, so they remain valid even if the UAsset is
	// later GC'd while this proxy is still in flight on the render thread.
	PositionsSRVCached =
		AssetPtr->GetPositionsSRV(PosMinCMCached, PosScaleCMCached);
	ColorsSRVCached = AssetPtr->GetColorsSRV();
	SphericalHarmonicsSRVCached = AssetPtr->GetSphericalHarmonicsSRV();
	CovariancesSRVCached = AssetPtr->GetCovariancesSRV();
	CovarianceScaleCM2Cached = AssetPtr->GetCovarianceScaleCM2();

	if (bIsSortingOnGPU)
	{
		Indices = FSplatGPUToGPUBuffer(
			NumSplatsCached, EPixelFormat::PF_R32_UINT);
	}
	else
	{
		CPUSorting = std::make_shared<FMultithreadedSortingBuffers>(
			NumSplatsCached);
	}

#if WITH_EDITOR
	// Build main wireframe mesh from the BodySetup's actual convex geometry.
	// In single-hull mode this is a single convex wrap; in multi-hull mode
	// it's the combined mesh of all individual hulls.
	{
		TArray<FDynamicMeshVertex> OutVerts;
		TArray<uint32> OutIndices;

		if (bForceSingleConvexHull && BodySetup)
		{
			// Use the convex elem data from BodySetup (single merged hull).
			for (const FKConvexElem& Elem : BodySetup->AggGeom.ConvexElems)
			{
				const uint32 VertexOffset = static_cast<uint32>(OutVerts.Num());
				for (const FVector& Vertex : Elem.VertexData)
				{
					OutVerts.Push(FVector3f(Vertex));
				}
				for (int32 Idx : Elem.IndexData)
				{
					OutIndices.Add(VertexOffset + static_cast<uint32>(Idx));
				}
			}
		}
		else
		{
			// Use the asset's combined collision mesh (all hulls appended).
			TConstArrayView<FVector3f> ConvexHullVertices =
				AssetPtr->GetConvexHullVertices();
			TConstArrayView<uint32> ConvexHullIndices =
				AssetPtr->GetConvexHullIndices();
			for (int32 Index = 0; Index < ConvexHullVertices.Num(); ++Index)
			{
				OutVerts.Push(ConvexHullVertices[Index]);
			}
			OutIndices.Append(ConvexHullIndices.GetData(), ConvexHullIndices.Num());
		}

		NumConvexHullTris = OutIndices.Num() / 3;
		VertexBuffers.InitFromDynamicVertex(&VertexFactory, OutVerts);
		IndexBuffer.Indices.SetNumUninitialized(OutIndices.Num());
		for (int32 Index = 0; Index < OutIndices.Num(); ++Index)
		{
			IndexBuffer.Indices[Index] = OutIndices[Index];
		}
		BeginInitResource(&IndexBuffer);
	}

	for (const FSplatCollisionHull& Hull : AssetPtr->GetCollisionHulls())
	{
		if (bForceSingleConvexHull)
		{
			break; // skip per-hull colored render data
		}

		if (Hull.Vertices.Num() < 4 || Hull.Indices.Num() < 3)
		{
			continue;
		}

		TUniquePtr<FEditorCollisionHullRenderData> HullRenderData =
			MakeUnique<FEditorCollisionHullRenderData>(GetScene().GetFeatureLevel());
		HullRenderData->NumPrimitives = Hull.Indices.Num() / 3;

		TArray<FDynamicMeshVertex> HullVerts;
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			HullVerts.Push(Vertex);
		}
		HullRenderData->VertexBuffers.InitFromDynamicVertex(
			&HullRenderData->VertexFactory,
			HullVerts);

		HullRenderData->IndexBuffer.Indices.SetNumUninitialized(Hull.Indices.Num());
		for (int32 Index = 0; Index < Hull.Indices.Num(); ++Index)
		{
			HullRenderData->IndexBuffer.Indices[Index] = Hull.Indices[Index];
		}
		BeginInitResource(&HullRenderData->IndexBuffer);
		CollisionHullRenderData.Add(MoveTemp(HullRenderData));
	}

	Name = Component.GetOwner()->GetActorLabel();
#else
	Name = Component.GetOwner()->GetName();
#endif
}

#if WITH_EDITOR
FPrimitiveViewRelevance
FSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result{};

	// Even with WITH_EDITOR guard, Unreal generally checks GIsEditor, so do the
	// same.
	if (GIsEditor)
	{
		// We always draw in Editor, as this is used to select the Splat Actor.
		Result.bDrawRelevance = IsShown(View);
		// Triggers a call to GetDynamicMeshElements().
		Result.bDynamicRelevance = true;
		// Enables Editor highlighting / selection outline.
		Result.bEditorStaticSelectionRelevance = (IsSelected() || IsHovered());
	}

	return Result;
}
#endif

void FSplatSceneProxy::CreateRenderThreadResources(
	FRHICommandListBase& RHICmdList)
{
	if (bIsSortingOnGPU)
	{
		Indices->InitRHI(RHICmdList);
	}
	else
	{
		CPUSorting->InitResources_RenderThread(RHICmdList);
	}
	Transforms.InitRHI(RHICmdList);

	check(GEngine);
	USplatSubsystem* Subsystem = GEngine->GetEngineSubsystem<USplatSubsystem>();
	check(Subsystem);
	Subsystem->RegisterSplat_RenderThread(this);
}

void FSplatSceneProxy::DestroyRenderThreadResources()
{
	check(GEngine);
	USplatSubsystem* Subsystem = GEngine->GetEngineSubsystem<USplatSubsystem>();
	check(Subsystem);
	Subsystem->UnregisterSplat_RenderThread(this);

	if (bIsSortingOnGPU)
	{
		Indices->ReleaseResource();
	}
	else
	{
		CPUSorting->ReleaseResources();
	}

	Transforms.ReleaseResource();

#if WITH_EDITOR
	VertexFactory.ReleaseResource();

	VertexBuffers.PositionVertexBuffer.ReleaseResource();
	VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
	VertexBuffers.ColorVertexBuffer.ReleaseResource();

	IndexBuffer.ReleaseResource();

	for (TUniquePtr<FEditorCollisionHullRenderData>& HullRenderData :
	     CollisionHullRenderData)
	{
		HullRenderData->VertexFactory.ReleaseResource();
		HullRenderData->VertexBuffers.PositionVertexBuffer.ReleaseResource();
		HullRenderData->VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
		HullRenderData->VertexBuffers.ColorVertexBuffer.ReleaseResource();
		HullRenderData->IndexBuffer.ReleaseResource();
	}
	CollisionHullRenderData.Reset();
#endif
}

#if WITH_EDITOR
void FSplatSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	class FMeshElementCollector& Collector) const
{
	check(GEngine);
	check(BodySetup);

	if (GIsEditor)
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			/**
			 * Collision Views.
			 *
			 * Collision: Show > Collision.
			 * CollisionPawn: View Mode > Player Collision.
			 * CollisionVisibility: View Mode > Visibility Collision.
			 */
			const bool bDrawPawnCollision =
				ViewFamily.EngineShowFlags.CollisionPawn;
			const bool bDrawVisCollision =
				ViewFamily.EngineShowFlags.CollisionVisibility;
			const bool bDrawCollisionOverlay =
				ViewFamily.EngineShowFlags.Collision;

			const bool bIsCollisionView = AllowDebugViewmodes() &&
			                              IsCollisionEnabled() &&
			                              bDrawPawnCollision;
			const bool bIsWireframeView =
				AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

			if (bIsCollisionView)
			{
				FLinearColor SelectionColor =
					GetSelectionColor(EditorColor, IsSelected(), IsHovered());

				TObjectPtr<UMaterial> Material;

				// If overlay is active, collisions become wireframe.
				const bool bDrawSolid = !bDrawCollisionOverlay;
				if (bDrawSolid)
				{
					Material = GEngine->ShadedLevelColorationUnlitMaterial;
				}
				else
				{
					Material = GEngine->WireframeMaterial;
				}

				// Note: This will be registered for deletion within
				// RegisterOneFrameMaterialProxy().
				FColoredMaterialRenderProxy* CollisionMaterialInstance =
					new FColoredMaterialRenderProxy(
						Material->GetRenderProxy(), SelectionColor);
				Collector.RegisterOneFrameMaterialProxy(
					CollisionMaterialInstance);
				BodySetup->AggGeom.GetAggGeom(
					FTransform(GetLocalToWorld()),
					SelectionColor.ToFColor(false),
					CollisionMaterialInstance,
					false,
					bDrawSolid,
					AlwaysHasVelocity(),
					ViewIndex,
					Collector);
			}

			/**
			 * Wireframe: View Mode > Wireframe.
			 */
			else if (bIsWireframeView)
			{
				const bool bShowColoredHulls =
					USplatSettings::ShowColoredCollisionHullsInEditor() &&
					!CollisionHullRenderData.IsEmpty();

				if (bShowColoredHulls)
				{
					for (int32 HullIndex = 0; HullIndex < CollisionHullRenderData.Num();
					     ++HullIndex)
					{
						const FEditorCollisionHullRenderData& HullRenderData =
							*CollisionHullRenderData[HullIndex];

						FColoredMaterialRenderProxy* WireframeMaterialInstance =
							new FColoredMaterialRenderProxy(
								GEngine->WireframeMaterial->GetRenderProxy(),
								GetSelectionColor(
									MakeHullDebugColor(HullIndex),
									IsSelected(),
									IsHovered(),
									false));
						Collector.RegisterOneFrameMaterialProxy(
							WireframeMaterialInstance);

						FMeshBatch& Mesh = Collector.AllocateMesh();
						Mesh.bDisableBackfaceCulling = true;
						Mesh.LODIndex = 0;
						Mesh.MaterialRenderProxy = WireframeMaterialInstance;
						Mesh.bUseWireframeSelectionColoring = IsSelected();
						Mesh.VertexFactory = &HullRenderData.VertexFactory;
						Mesh.bWireframe = true;

						FMeshBatchElement& BatchElement = Mesh.Elements[0];
						BatchElement.FirstIndex = 0;
						BatchElement.IndexBuffer = &HullRenderData.IndexBuffer;
						BatchElement.NumPrimitives = HullRenderData.NumPrimitives;

						Collector.AddMesh(ViewIndex, Mesh);
					}
				}
				else
				{
					FLinearColor ViewWireframeColor =
						ViewFamily.EngineShowFlags.ActorColoration
							? GetPrimitiveColor()
							: GetWireframeColor();

					FColoredMaterialRenderProxy* WireframeMaterialInstance =
						new FColoredMaterialRenderProxy(
							GEngine->WireframeMaterial->GetRenderProxy(),
							GetSelectionColor(
								ViewWireframeColor,
								IsSelected(),
								IsHovered(),
								false));
					Collector.RegisterOneFrameMaterialProxy(
						WireframeMaterialInstance);

					FMeshBatch& Mesh = Collector.AllocateMesh();
					Mesh.bDisableBackfaceCulling = true; // In case we're inside.
					Mesh.LODIndex = 0;
					Mesh.MaterialRenderProxy = WireframeMaterialInstance;
					Mesh.bUseWireframeSelectionColoring = IsSelected();
					Mesh.VertexFactory = &VertexFactory;
					Mesh.bWireframe = true;

					FMeshBatchElement& BatchElement = Mesh.Elements[0];
					BatchElement.FirstIndex = 0;
					BatchElement.IndexBuffer = &IndexBuffer;
					BatchElement.NumPrimitives = NumConvexHullTris;

					Collector.AddMesh(ViewIndex, Mesh);
				}
			}

			/**
			 * If no special display, render an invisible mesh to enable mouse
			 * selection.
			 */
			else
			{
				/**
				 * Note: I haven't confirmed this is deleted by Unreal, but
				 * other scene proxies do the same thing.
				 */
				FColoredMaterialRenderProxy* HullMaterialInstance =
					new FColoredMaterialRenderProxy(
						GEngine->GeomMaterial->GetRenderProxy(),
						FLinearColor(0, 0, 0, 0));
				FMeshBatch& Mesh = Collector.AllocateMesh();
				Mesh.bDisableBackfaceCulling = true; // In case we're inside.
				Mesh.LODIndex = 0;
				Mesh.MaterialRenderProxy = HullMaterialInstance;
				Mesh.VertexFactory = &VertexFactory;

				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.FirstIndex = 0;
				BatchElement.IndexBuffer = &IndexBuffer;
				BatchElement.NumPrimitives = NumConvexHullTris;

				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}
}
#endif

bool FSplatSceneProxy::IsVisible(const FSceneView& View) const
{
	bool bIsShown = IsShown(&View);
	bool bIsInScene = &GetScene() == View.Family->Scene;
	bool bIsVisible = bIsShown && bIsInScene;

#if WITH_EDITOR
	const FEngineShowFlags& Flags = View.Family->EngineShowFlags;
	bool bIsWireframe = Flags.Wireframe;
	bool bIsCollision =
		Flags.Collision || Flags.CollisionPawn || Flags.CollisionVisibility;

	return !bIsWireframe && !bIsCollision && bIsVisible;
#else
	return bIsVisible;
#endif
}

void FSplatSceneProxy::TryEnqueueSort(
	const FVector3f& OriginCM, const FVector3f& Forward)
{
	check(!bIsSortingOnGPU);
	check(CPUSorting);

	// The sort task copies a TConstArrayView pointing at the asset's CPU
	// position buffer, so skip if the asset has been GC'd to avoid the task
	// dereferencing freed memory.
	USplatAsset* AssetPtr = Asset.Get();
	if (!AssetPtr)
	{
		return;
	}

	if (!CPUSorting->IsReadyForSorting())
	{
		return;
	}

	// This launches a new sorting task which will `delete` itself once finished.
	// This is necessary as we otherwise must wait on the task to be completed in
	// our destructor before it can be deleted.
	// See AsyncWork.h.
	(new FAutoDeleteAsyncTask<FCPUSortingTask>(
		 AssetPtr->GetPositions(),
		 CPUSorting,
		 OriginCM,
		 Forward,
		 FMatrix44f(GetLocalToWorld())))
		->StartBackgroundTask();
}

} // namespace PICO::Splat