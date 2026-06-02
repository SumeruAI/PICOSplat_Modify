/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "SplatAsset.h"

#include "SplatAssetFactory.generated.h"

/**
 * Importer for 3DGS `.ply` files.
 */
UCLASS()
class USplatAssetFactory final : public UFactory, public FReimportHandler
{
	GENERATED_BODY()

public:
	/**
	 * Registers `.ply` file type for import as a Gaussian Splat asset.
	 */
	USplatAssetFactory();

	/**
	 * Imports splat `.ply` files into `USplatAsset`s.
	 */
	virtual UObject* FactoryCreateBinary(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		UObject* Context,
		const TCHAR* Type,
		const uint8*& Buffer,
		const uint8* BufferEnd,
		FFeedbackContext* Warn) override;

	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	virtual int32 GetPriority() const override;
};