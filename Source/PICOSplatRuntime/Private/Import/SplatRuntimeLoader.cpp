#include "Import/SplatRuntimeLoader.h"

#include <span>

#include "Misc/FileHelper.h"
#include "import/ply/splat_ply_conversion.h"
#include "import/ply/splat_ply_parsing.h"

using namespace import;
using import::GetPropertyFn;
using import::Metadata;
using import::ParseSplatFn;
using import::ply::SplatParserPly;

bool FSplatRuntimeLoader::LoadFromPLYMemory(
	TConstArrayView<uint8> Bytes,
	FRuntimeSplatBuildData& OutData,
	FString& OutError)
{
	SplatParserPly Parser;
	Metadata PLYMetadata;

	std::span<const uint8_t> BufferView(Bytes.GetData(), Bytes.Num());
	if (!Parser.parse_metadata(BufferView, PLYMetadata))
	{
		OutError = TEXT("Failed to parse PLY metadata.");
		return false;
	}

	if (!ply::validate_metadata(PLYMetadata))
	{
		OutError = TEXT("Invalid PLY metadata.");
		return false;
	}

	OutData.PositionsMeters.SetNumUninitialized(PLYMetadata.num_splats);
	OutData.Rotations.SetNumUninitialized(PLYMetadata.num_splats);
	OutData.ScalesMeters.SetNumUninitialized(PLYMetadata.num_splats);
	OutData.Colors.SetNumUninitialized(PLYMetadata.num_splats);
	const size_t SphericalHarmonicCoeffCount =
		ply::get_spherical_harmonic_coeff_count(PLYMetadata);
	if (SphericalHarmonicCoeffCount > 0)
	{
		OutData.SphericalHarmonics.SetNumZeroed(
			PLYMetadata.num_splats *
			FRuntimeSplatBuildData::MaxSphericalHarmonicCoefficients);
	}

	ParseSplatFn ParseSplat =
		[P = std::span<FVector3f>(
			 OutData.PositionsMeters.GetData(), OutData.PositionsMeters.Num()),
	     R = std::span<FQuat4f>(
			 OutData.Rotations.GetData(), OutData.Rotations.Num()),
	     S = std::span<FVector3f>(
			 OutData.ScalesMeters.GetData(), OutData.ScalesMeters.Num()),
	     C = std::span<FColor>(OutData.Colors.GetData(), OutData.Colors.Num()),
	     SH = std::span<FVector4f>(
			 OutData.SphericalHarmonics.GetData(),
			 OutData.SphericalHarmonics.Num()),
	     SphericalHarmonicCoeffCount](
			uint32_t Index, GetPropertyFn Get)
	{
		ply::convert_splat<FVector3f, FQuat4f, FColor, FVector4f>(
			Index,
			Get,
			P,
			R,
			S,
			C,
			SH,
			SphericalHarmonicCoeffCount);
	};

	if (!Parser.parse_data(ParseSplat))
	{
		OutError = TEXT("Failed to parse PLY splat data.");
		return false;
	}

	return true;
}

bool FSplatRuntimeLoader::LoadFromPLYFile(
	const FString& FilePath,
	FRuntimeSplatBuildData& OutData,
	FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		OutError = FString::Printf(TEXT("Failed to read file: %s"), *FilePath);
		return false;
	}

	return LoadFromPLYMemory(Bytes, OutData, OutError);
}
