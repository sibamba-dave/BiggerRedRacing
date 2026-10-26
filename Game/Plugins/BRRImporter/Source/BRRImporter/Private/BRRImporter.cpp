#include "BRRImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BRRArchive.h"
#include "BRRWorld.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRImport, Log, All);

namespace
{
	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone;

	// Creates (or recycles) the package for DestPath and moves any previous asset of that name out
	// of the way, so re-importing overwrites cleanly.
	UPackage* MakePackage(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		Package->FullyLoad();
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *AssetName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		}
		return Package;
	}

	bool SaveAsset(UObject* Asset)
	{
		UPackage* Package = Asset->GetPackage();
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		const FString Extension = Asset->IsA<UWorld>() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
		FSavePackageArgs Args;
		Args.TopLevelFlags = AssetFlags;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_CLOG(!bSaved, LogBRRImport, Error, TEXT("Failed to save %s"), *File);
		return bSaved;
	}

	template <typename T>
	T* NewAsset(const FString& PackageName)
	{
		UPackage* Package = MakePackage(PackageName);
		return NewObject<T>(Package, *FPackageName::GetLongPackageAssetName(PackageName), AssetFlags);
	}

	// bMips: a mip chain for the remastered look's filtered sampling (the classic look reads only
	// the top mip); the palette and sky strips are lookup tables and have none.
	UTexture2D* MakeTexture(const FString& PackageName, const FBRRImage& Image, bool bMips = false)
	{
		UTexture2D* Texture = NewAsset<UTexture2D>(PackageName);
		// FColor is BGRA in memory, which is exactly TSF_BGRA8.
		Texture->Source.Init(Image.Width, Image.Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Image.Pixels.GetData()));
		Texture->SRGB = true;
		Texture->Filter = TF_Nearest;
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->MipGenSettings = bMips ? TMGS_SimpleAverage : TMGS_NoMipmaps;
		// Never streamed: the classic look reads the top mip, and a streamed texture serves a blurred
		// lower one until the top arrives (82% of a classic frame was off the palette).
		Texture->NeverStream = true;
		Texture->CompressionSettings = TC_EditorIcon; // uncompressed: palette art must stay exact
		Texture->PostEditChange();
		SaveAsset(Texture);
		return Texture;
	}

	// The upscaled pages for the remastered look (Tools/upscale_textures.py). Every page the
	// importer meets is written to Saved/BRRAtlases/<terrain|pages>/<hash>.png, named by its pixels
	// so a page several levels share is upscaled once; the tool leaves <hash>.png (the colour, 4x)
	// and <hash>_nr.png (tangent-space normal in RGB, roughness variation about one half in A) in
	// Tools/upscale/cache, and the next import picks them up. Classic never reads them.
	FString PageHash(const FBRRImage& Image)
	{
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Image.Pixels.GetData(), Image.Pixels.Num() * sizeof(FColor), Digest);
		return BytesToHex(Digest, 8);
	}

	FString UpscaleCacheDir() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../Tools/upscale/cache")); }

	void DumpPage(const FBRRImage& Image, const FString& Hash, bool bTerrain)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("BRRAtlases") / (bTerrain ? TEXT("terrain") : TEXT("pages")) / Hash + TEXT(".png");
		if (IFileManager::Get().FileExists(*Path)) return;
		FImageUtils::SaveImageByExtension(*Path, FImageView(Image.Pixels.GetData(), Image.Width, Image.Height));
	}

	// An upscaled page as a texture: the colour in sRGB, the normal/roughness map as raw data.
	UTexture2D* MakeUpscaledTexture(const FString& PackageName, const FString& File, bool bColour)
	{
		FImage Image;
		if (!FImageUtils::LoadImage(*File, Image)) return nullptr;
		Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace); // the bytes as written, no gamma change
		UTexture2D* Texture = NewAsset<UTexture2D>(PackageName);
		Texture->Source.Init(Image.SizeX, Image.SizeY, 1, 1, TSF_BGRA8, Image.RawData.GetData());
		Texture->SRGB = bColour;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->CompressionSettings = TC_Default;
		Texture->PostEditChange();
		SaveAsset(Texture);
		return Texture;
	}

	// The two classic masters: unlit, palette colours shown as-is. Textured is masked on the atlas
	// alpha (palette index 0); Flat takes the vertex colour.
	struct FMasters
	{
		UMaterial* Textured = nullptr;
		UMaterial* Flat = nullptr;
		UMaterial* Sky = nullptr;      // the original's banded sky, from an 18x1 palette strip
		UMaterial* Display = nullptr;  // replaces the tonemapper: scene colour out as sRGB, untouched
		UTexture2D* FlatDetail = nullptr; // a page's normal/roughness map where it has no upscale: flat, average
	};

	UMaterialExpressionCustom* MakeCustom(UMaterial* Mat, const FString& Code, ECustomMaterialOutputType Output,
		TArray<TPair<FName, UMaterialExpression*>> Inputs)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(
			UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionCustom::StaticClass(), -500, 0));
		Custom->Code = Code;
		Custom->OutputType = Output;
		Custom->Inputs.Reset();
		for (const TPair<FName, UMaterialExpression*>& In : Inputs)
		{
			FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
			Input.InputName = In.Key;
			Input.Input.Connect(0, In.Value);
		}
		return Custom;
	}

	// The original's view distance: a terrain cell is drawn while its centre is nearer than
	// MPC_BRR.DrawDistance (cm, 0 = everything) in view depth, so the far edge comes and goes a whole
	// cell at a time. 1 inside, 0 beyond. The race sets the distance (brr.ViewCells).
	UMaterialExpression* MakeDrawRange(UMaterial* Mat, UMaterialParameterCollection* Collection)
	{
		auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
		UMaterialExpression* WorldPos = Make(UMaterialExpressionWorldPosition::StaticClass(), -1100, 300);
		UMaterialExpression* Camera = Make(UMaterialExpressionCameraPositionWS::StaticClass(), -1100, 400);
		auto* Ahead = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -1300, 500));
		Ahead->Constant = FLinearColor(0, 0, 1); // view space looks down +Z
		auto* Forward = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -1100, 500));
		Forward->Input.Connect(0, Ahead);
		Forward->TransformSourceType = TRANSFORMSOURCE_View;
		Forward->TransformType = TRANSFORM_World;
		auto* Range = Cast<UMaterialExpressionCollectionParameter>(Make(UMaterialExpressionCollectionParameter::StaticClass(), -1100, 600));
		Range->Collection = Collection;
		Range->ParameterName = TEXT("DrawDistance");
		Range->ParameterId = Collection->GetParameterId(Range->ParameterName);
		const double Cell = BRR::TerrainCellCm;
		return MakeCustom(Mat, FString::Printf(TEXT(
			"if (Range <= 0) return 1;\n"
			"float2 c = (floor(WorldPos.xy / %.1f) + 0.5) * %.1f;\n"
			"float d = dot(float3(c, WorldPos.z) - Camera, normalize(Forward));\n"
			"return d < Range ? 1 : 0;"), Cell, Cell), CMOT_Float1,
			{ { TEXT("WorldPos"), WorldPos }, { TEXT("Camera"), Camera }, { TEXT("Forward"), Forward }, { TEXT("Range"), Range } });
	}

	UMaterialExpression* MakeCollectionValue(UMaterial* Mat, UMaterialParameterCollection* Collection, FName Name, int32 X, int32 Y)
	{
		auto* Value = Cast<UMaterialExpressionCollectionParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionCollectionParameter::StaticClass(), X, Y));
		Value->Collection = Collection;
		Value->ParameterName = Name;
		Value->ParameterId = Collection->GetParameterId(Name);
		return Value;
	}

	UMaterialExpressionCustom* MakeCustomOutputs(UMaterial* Mat, const FString& Code, ECustomMaterialOutputType Output,
		TArray<TTuple<FName, UMaterialExpression*, int32>> Inputs, int32 Y)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(
			UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionCustom::StaticClass(), -250, Y));
		Custom->Code = Code;
		Custom->OutputType = Output;
		Custom->Inputs.Reset();
		for (const TTuple<FName, UMaterialExpression*, int32>& In : Inputs)
		{
			FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
			Input.InputName = In.Get<0>();
			Input.Input.Connect(In.Get<2>(), In.Get<1>());
		}
		return Custom;
	}

	// The two looks off one material, switched at runtime by MPC_BRR.Remaster (ABRRRaceGameMode):
	// classic (0) is the original's colour as emissive on a black, unshining surface - unlit, the
	// palette exact; remastered (1) is the same colour as a lit base colour.
	//
	// The remastered look also shades the terrain's liquids (EBRRLiquid, carried in the vertex
	// alpha by MakeMesh: 1 ground, 2/3 water, 1/3 ice, 0 lava): water as a dark, glassy surface
	// under moving ripples, which Lumen's reflections show the sky and the banks in; ice as a
	// smooth, shining one; lava glowing. The classic look ignores it.
	//
	// Detail (float4) is the surface's own relief: a tangent-space normal and a roughness variation
	// about one half, from the upscaled page's normal/roughness map - flat and average without one.
	void ConnectLook(UMaterial* Mat, UMaterialParameterCollection* Collection, UMaterialExpression* Colour, int32 ColourOutput,
		UMaterialExpression* Detail)
	{
		auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
		UMaterialExpression* Remaster = MakeCollectionValue(Mat, Collection, TEXT("Remaster"), -500, 400);
		UMaterialExpression* Vertex = Make(UMaterialExpressionVertexColor::StaticClass(), -800, 700);
		UMaterialExpressionCustom* Kind = MakeCustomOutputs(Mat, TEXT("return R > 0.5 ? round((1 - saturate(A)) * 3) : 0;"), CMOT_Float1,
			{ MakeTuple(FName(TEXT("A")), Vertex, 4), MakeTuple(FName(TEXT("R")), Remaster, 0) }, 700);
		// What the remastered surface is made of where it is not a liquid (ESurface: the scenery's
		// matte default, a vehicle's paint, its tyres).
		auto Scalar = [&](const TCHAR* Name, float Default, int32 Y)
		{
			auto* P = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -800, Y));
			P->ParameterName = Name;
			P->DefaultValue = Default;
			return P;
		};
		UMaterialExpression* Rough = Scalar(TEXT("Roughness"), 0.75f, 1000);
		UMaterialExpression* Shine = Scalar(TEXT("Specular"), 0.35f, 1100);
		auto Part = [&](const TCHAR* Code, ECustomMaterialOutputType Type, int32 Y)
		{
			return MakeCustomOutputs(Mat, FString::Printf(TEXT("int k = (int)K;\n%s"), Code), Type,
				{ MakeTuple(FName(TEXT("C")), Colour, ColourOutput), MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("K")), (UMaterialExpression*)Kind, 0),
				  MakeTuple(FName(TEXT("Rough")), Rough, 0), MakeTuple(FName(TEXT("Shine")), Shine, 0), MakeTuple(FName(TEXT("D")), Detail, 0) }, Y);
		};
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return C.rgb * (1 - R) + (k == 3 ? C.rgb * 16.0 : 0);"), CMOT_Float3, 0), TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return C.rgb * R * (k == 1 ? 0.35 : k == 3 ? 0.02 : 1.0);"), CMOT_Float3, 100), TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return R * (k == 1 || k == 2 ? 0.5 : Shine);"), CMOT_Float1, 200), TEXT(""), MP_Specular);
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return k == 1 ? 0.03 : k == 2 ? 0.1 : k == 3 ? 0.55 : saturate(Rough + (D.w - 0.5));"), CMOT_Float1, 300), TEXT(""), MP_Roughness);

		// The ripples: four travelling waves, a few metres long, summed into a world-space slope
		// and handed to the material in tangent space (the atlas tiles turn and mirror from cell to
		// cell, so the waves are laid out in the world, not in UV).
		UMaterialExpression* WorldPos = Make(UMaterialExpressionWorldPosition::StaticClass(), -1100, 800);
		UMaterialExpression* Time = Make(UMaterialExpressionTime::StaticClass(), -1100, 900);
		UMaterialExpressionCustom* Ripples = MakeCustomOutputs(Mat, TEXT(
			"const float4 W[4] = { float4(0.8, 0.6, 520, 0.9), float4(-0.5, 0.87, 310, 1.3), float4(0.2, -0.98, 190, 1.7), float4(-0.93, -0.36, 110, 2.3) };\n"
			"float2 g = 0;\n"
			"for (int i = 0; i < 4; i++)\n"
			"{\n"
			"  float k = 6.2831853 / W[i].z;\n"
			"  g += W[i].xy * (0.045 * cos(dot(W[i].xy, P.xy) * k - T * W[i].w));\n"
			"}\n"
			"return normalize(float3(-g, 1));"), CMOT_Float3,
			{ MakeTuple(FName(TEXT("P")), WorldPos, 0), MakeTuple(FName(TEXT("T")), Time, 0) }, 800);
		auto* ToTangent = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -500, 800));
		ToTangent->Input.Connect(0, Ripples);
		ToTangent->TransformSourceType = TRANSFORMSOURCE_World;
		ToTangent->TransformType = TRANSFORM_Tangent;
		UMaterialExpressionCustom* Normal = MakeCustomOutputs(Mat, TEXT("return K == 1 ? normalize(N) : R > 0.5 ? normalize(D.xyz) : float3(0, 0, 1);"), CMOT_Float3,
			{ MakeTuple(FName(TEXT("N")), (UMaterialExpression*)ToTangent, 0), MakeTuple(FName(TEXT("K")), (UMaterialExpression*)Kind, 0),
			  MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("D")), Detail, 0) }, 900);
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);
	}

	FMasters MakeMasters(const FString& Root)
	{
		FMasters M;
		UMaterialParameterCollection* Collection = NewAsset<UMaterialParameterCollection>(Root / TEXT("Materials/MPC_BRR"));
		{
			FCollectionScalarParameter Distance;
			Distance.ParameterName = TEXT("DrawDistance");
			Distance.DefaultValue = 0.f; // the editor shows the whole map
			Collection->ScalarParameters.Add(Distance);
			FCollectionScalarParameter Remaster;
			Remaster.ParameterName = TEXT("Remaster");
			Remaster.DefaultValue = 0.f;
			Collection->ScalarParameters.Add(Remaster);
			Collection->PostEditChange();
			SaveAsset(Collection);
		}
		{
			FBRRImage Flat;
			Flat.Width = Flat.Height = 4;
			Flat.Pixels.Init(FColor(128, 128, 255, 128), 16);
			UTexture2D* Texture = NewAsset<UTexture2D>(Root / TEXT("Textures/T_BRR_FlatDetail"));
			Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Flat.Pixels.GetData()));
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Default;
			Texture->PostEditChange();
			SaveAsset(Texture);
			M.FlatDetail = Texture;
		}
		{
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Textured"));
			Mat->SetShadingModel(MSM_DefaultLit);
			Mat->BlendMode = BLEND_Masked;
			// Classic samples the top mip with the texture's own nearest filter; remastered with
			// the world's filtering (trilinear/anisotropic) across the mips.
			UTexture2D* Default = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			auto* Classic = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, 0));
			Classic->ParameterName = TEXT("Atlas");
			Classic->Texture = Default;
			Classic->MipValueMode = TMVM_MipLevel;
			Classic->ConstMipValue = 0;
			auto* Smooth = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, 250));
			// Remastered: the upscaled page where there is one (AtlasHD; else the page itself), and
			// its normal/roughness map (AtlasDetail).
			Smooth->ParameterName = TEXT("AtlasHD");
			Smooth->Texture = Default;
			Smooth->SamplerSource = SSM_Wrap_WorldGroupSettings;
			auto* DetailMap = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, 1300));
			DetailMap->ParameterName = TEXT("AtlasDetail");
			DetailMap->Texture = M.FlatDetail;
			DetailMap->SamplerType = SAMPLERTYPE_LinearColor;
			DetailMap->SamplerSource = SSM_Wrap_WorldGroupSettings;
			UMaterialExpressionCustom* Detail = MakeCustomOutputs(Mat, TEXT("return float4(T.rgb * 2 - 1, T.a);"), CMOT_Float4,
				{ MakeTuple(FName(TEXT("T")), (UMaterialExpression*)DetailMap, 5) }, 1300);
			UMaterialExpression* Remaster = MakeCollectionValue(Mat, Collection, TEXT("Remaster"), -800, 500);
			UMaterialExpressionCustom* Colour = MakeCustomOutputs(Mat, TEXT("return lerp(A, B, R);"), CMOT_Float4,
				{ MakeTuple(FName(TEXT("A")), (UMaterialExpression*)Classic, 5), MakeTuple(FName(TEXT("B")), (UMaterialExpression*)Smooth, 5), MakeTuple(FName(TEXT("R")), Remaster, 0) }, 500);
			ConnectLook(Mat, Collection, Colour, 0, Detail);
			auto* Mask = MakeCustomOutputs(Mat, TEXT("return C.a * D;"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("C")), (UMaterialExpression*)Colour, 0), MakeTuple(FName(TEXT("D")), MakeDrawRange(Mat, Collection), 0) }, 600);
			UMaterialEditingLibrary::ConnectMaterialProperty(Mask, TEXT(""), MP_OpacityMask);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Textured = Mat;
		}
		{
			// Band index from the view direction's elevation, as ORR's sky shader picks it.
			FString Bands;
			for (int32 B = 0; B < 16; ++B) Bands += FString::Printf(TEXT("%s%.2f"), B ? TEXT(",") : TEXT(""), BRR::SkyBandDegrees[B]);
			const FString Code = FString::Printf(TEXT(
				"const float B[16] = {%s};\n"
				"float e = degrees(asin(clamp(-View.z, -1.0, 1.0)));\n"
				"float i = 15.0;\n"
				"if (e < 0.0) { i = (-e >= %.2f) ? 17.0 : 16.0; }\n"
				"else { for (int k = 0; k < 16; k++) { if (e >= B[k]) { i = k; break; } } }\n"
				"return float2((i + 0.5) / 18.0, 0.5);"), *Bands, BRR::SkyGroundBandDegrees);
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Sky"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->TwoSided = true;
			Mat->bIsSky = true;
			UMaterialExpression* View = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionCameraVectorWS::StaticClass(), -700, 0);
			UMaterialExpressionCustom* Uv = MakeCustom(Mat, Code, CMOT_Float2, { { TEXT("View"), View } });
			auto* Tex = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
			Tex->ParameterName = TEXT("Sky");
			Tex->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			Tex->Coordinates.Connect(0, Uv);
			UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("RGB"), MP_EmissiveColor);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Sky = Mat;
		}
		{
			// Unlit palette colours reach here exactly as authored; encode them for the display
			// with no tone curve, so a pixel on screen is the original's palette entry. Snapped to
			// the 8-bit step and lifted a quarter of one: the scene colour is fp16, and somewhere
			// on the way out a value a hair under a step is truncated (208 came out as 207, see
			// Tools/palette_check.py), which rounding and truncation both survive from here.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Display"));
			Mat->MaterialDomain = MD_PostProcess;
			Mat->BlendableLocation = BL_ReplacingTonemapper;
			auto* Scene = Cast<UMaterialExpressionSceneTexture>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionSceneTexture::StaticClass(), -700, 0));
			Scene->SceneTextureId = PPI_PostProcessInput0;
			UMaterialExpressionCustom* Encode = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Linear.rgb);\n"
				"float3 lo = c * 12.92;\n"
				"float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
				"float3 s = lerp(hi, lo, step(c, 0.0031308));\n"
				"return saturate((floor(s * 255.0 + 0.5) + 0.25) / 255.0);"), CMOT_Float3, { { TEXT("Linear"), Scene } });
			UMaterialEditingLibrary::ConnectMaterialProperty(Encode, TEXT(""), MP_EmissiveColor);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Display = Mat;
		}
		{
			// The vertex colour holds the palette entry's own bytes (a static mesh build sRGB-
			// encodes it, and the vertex factory hands the bytes over raw), so it is decoded here
			// as a texture sample would be. Read as linear, every flat face came out too bright:
			// (32, 32, 32) showed as (99, 99, 99).
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Flat"));
			Mat->SetShadingModel(MSM_DefaultLit);
			UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVertexColor::StaticClass(), -500, 0);
			UMaterialExpressionCustom* Decode = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));"), CMOT_Float3, { { TEXT("Srgb"), Colour } });
			auto* Detail = Cast<UMaterialExpressionConstant4Vector>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionConstant4Vector::StaticClass(), -800, 1300));
			Detail->Constant = FLinearColor(0, 0, 1, 0.5f); // flat colours have no relief
			ConnectLook(Mat, Collection, Decode, 0, Detail);
			Mat->BlendMode = BLEND_Masked;
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeDrawRange(Mat, Collection), TEXT(""), MP_OpacityMask);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Flat = Mat;
		}
		return M;
	}

	// What a mesh is made of, for the remastered look: the scenery's matte ground and decor, a
	// vehicle's glossy paint (the whole body: the original paints it all one way), its rubber tyres.
	enum class ESurface : uint8 { Ground, Paint, Rubber };

	// Per-level asset state shared by every mesh: one texture per used atlas page, one material
	// instance per (atlas, sidedness, surface).
	struct FLevelAssets
	{
		const FBRRWorld* World = nullptr;
		FString Dir;      // DestRoot/<LEVEL>
		FString Prefix;   // <LEVEL>
		FMasters Masters;
		TMap<int32, UTexture2D*> Textures;
		TSet<int32> TerrainAtlases; // pages the terrain draws from: a grid of tiles, upscaled tile by tile
		TMap<int32, TPair<UTexture2D*, UTexture2D*>> Upscaled; // colour, detail; null where not upscaled
		TMap<TTuple<int32, bool, ESurface>, UMaterialInstanceConstant*> Materials;

		UMaterialInstanceConstant* MaterialFor(int32 Atlas, bool bTwoSided, ESurface Surface = ESurface::Ground)
		{
			if (UMaterialInstanceConstant** Found = Materials.Find({ Atlas, bTwoSided, Surface })) return *Found;

			const FString Name = FString::Printf(TEXT("MI_%s_%s%s%s"), *Prefix,
				Atlas == INDEX_NONE ? TEXT("Flat") : *FString::Printf(TEXT("Atlas%02d"), Atlas), bTwoSided ? TEXT("_2S") : TEXT(""),
				Surface == ESurface::Paint ? TEXT("_Paint") : Surface == ESurface::Rubber ? TEXT("_Rubber") : TEXT(""));
			UMaterialInstanceConstant* MI = NewAsset<UMaterialInstanceConstant>(Dir / TEXT("Materials") / Name);
			MI->SetParentEditorOnly(Atlas == INDEX_NONE ? Masters.Flat : Masters.Textured);
			if (Atlas != INDEX_NONE)
			{
				UTexture2D*& Texture = Textures.FindOrAdd(Atlas);
				if (!Texture)
				{
					Texture = MakeTexture(Dir / TEXT("Textures") / FString::Printf(TEXT("T_%s_Atlas%02d"), *Prefix, Atlas), World->Atlases[Atlas], true);
				}
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Atlas")), Texture);
				TPair<UTexture2D*, UTexture2D*>* Up = Upscaled.Find(Atlas);
				if (!Up)
				{
					const FBRRImage& Page = World->Atlases[Atlas];
					const FString Hash = PageHash(Page);
					DumpPage(Page, Hash, TerrainAtlases.Contains(Atlas));
					const FString Base = UpscaleCacheDir() / Hash;
					const FString Stem = Dir / TEXT("Textures") / FString::Printf(TEXT("T_%s_Atlas%02d"), *Prefix, Atlas);
					TPair<UTexture2D*, UTexture2D*> New(nullptr, nullptr);
					if (IFileManager::Get().FileExists(*(Base + TEXT(".png"))) && IFileManager::Get().FileExists(*(Base + TEXT("_nr.png"))))
					{
						New.Key = MakeUpscaledTexture(Stem + TEXT("_HD"), Base + TEXT(".png"), true);
						New.Value = MakeUpscaledTexture(Stem + TEXT("_Detail"), Base + TEXT("_nr.png"), false);
					}
					Up = &Upscaled.Add(Atlas, New);
				}
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AtlasHD")), Up->Key ? Up->Key : Texture);
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AtlasDetail")), Up->Value ? Up->Value : Masters.FlatDetail);
			}
			if (bTwoSided)
			{
				MI->BasePropertyOverrides.bOverride_TwoSided = true;
				MI->BasePropertyOverrides.TwoSided = true;
			}
			// Paint: a fresh, glossy finish that Lumen's reflections show the world in. Rubber: dull.
			if (Surface == ESurface::Paint)
			{
				MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 0.3f);
				MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Specular")), 0.5f);
			}
			else if (Surface == ESurface::Rubber)
			{
				MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 0.9f);
				MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Specular")), 0.2f);
			}
			MI->PostEditChange();
			SaveAsset(MI);
			Materials.Add({ Atlas, bTwoSided, Surface }, MI);
			return MI;
		}
	};

	UStaticMesh* MakeMesh(FLevelAssets& Assets, const FString& Name, const FBRRMesh& Source, bool bComplexCollision,
		ESurface Surface = ESurface::Ground)
	{
		if (Source.NumTriangles() == 0) return nullptr;

		FMeshDescription Desc;
		FStaticMeshAttributes Attributes(Desc);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(1);

		const int32 Verts = Source.NumTriangles() * 3;
		Desc.ReserveNewVertices(Verts);
		Desc.ReserveNewVertexInstances(Verts);
		Desc.ReserveNewTriangles(Source.NumTriangles());

		UStaticMesh* Mesh = NewAsset<UStaticMesh>(Assets.Dir / TEXT("Meshes") / Name);
		for (const FBRRMeshSection& Section : Source.Sections)
		{
			UMaterialInstanceConstant* Material = Assets.MaterialFor(Section.Atlas, Section.bTwoSided, Surface);
			const FName Slot = Material->GetFName();
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, Slot, Slot));
			const FPolygonGroupID Group = Desc.CreatePolygonGroup();
			SlotNames[Group] = Slot;

			for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
			{
				const FVector3f A = Section.Vertices[T].Position;
				const FVector3f Normal = ((Section.Vertices[T + 2].Position - A) ^ (Section.Vertices[T + 1].Position - A)).GetSafeNormal();
				TStaticArray<FVertexInstanceID, 3> Corners;
				for (int32 K = 0; K < 3; ++K)
				{
					const FBRRVertex& V = Section.Vertices[T + K];
					const FVertexID Vertex = Desc.CreateVertex();
					Positions[Vertex] = V.Position;
					const FVertexInstanceID Instance = Desc.CreateVertexInstance(Vertex);
					Normals[Instance] = Normal;
					UVs.Set(Instance, 0, V.UV);
					// The build sRGB-encodes the colour back to the palette bytes (M_BRR_Flat); the
					// alpha carries the liquid (ConnectLook).
					FLinearColor Colour(V.Color);
					const EBRRLiquid Liquid = V.Land <= 7 ? Assets.World->LiquidOf(V.Land) : EBRRLiquid::None;
					Colour.A = Liquid == EBRRLiquid::Water ? 2.f / 3.f : Liquid == EBRRLiquid::Ice ? 1.f / 3.f : Liquid == EBRRLiquid::Lava ? 0.f : 1.f;
					Colors[Instance] = FVector4f(Colour);
					Corners[K] = Instance;
				}
				Desc.CreateTriangle(Group, Corners);
			}
		}

		FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
		Model.BuildSettings.bRecomputeNormals = false;
		Model.BuildSettings.bRecomputeTangents = true;
		Model.BuildSettings.bRemoveDegenerates = false;
		Model.BuildSettings.bGenerateLightmapUVs = false;
		Model.BuildSettings.bUseFullPrecisionUVs = true;
		Mesh->CreateMeshDescription(0, MoveTemp(Desc));
		Mesh->CommitMeshDescription(0);
		Mesh->CreateBodySetup();
		Mesh->GetBodySetup()->CollisionTraceFlag = bComplexCollision ? CTF_UseComplexAsSimple : CTF_UseDefault;
		Mesh->Build(/*bInSilent=*/true);
		Mesh->PostEditChange();
		SaveAsset(Mesh);
		return Mesh;
	}

	struct FVehicleAssets
	{
		UStaticMesh* Body = nullptr;
		TArray<TPair<UStaticMesh*, FVector>> Wheels; // mesh, socket
		double GroundOffset = 0; // raise the origin by this to sit the lowest point on the ground
	};

	FVehicleAssets MakeVehicle(FLevelAssets& Assets, const FBRRVehicleMesh& Vehicle)
	{
		FVehicleAssets Out;
		Out.Body = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body"), *Vehicle.Name), Vehicle.Body, false, ESurface::Paint);
		double Lowest = Vehicle.Body.GetBounds().Min.Z;
		for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
		{
			const FBRRWheel& Wheel = Vehicle.Wheels[I];
			if (UStaticMesh* Mesh = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Wheel%d"), *Vehicle.Name, I), Wheel.Mesh, false, ESurface::Rubber))
			{
				Out.Wheels.Add({ Mesh, Wheel.Socket });
				Lowest = FMath::Min(Lowest, Wheel.Socket.Z + Wheel.Mesh.GetBounds().Min.Z);
			}
		}
		Out.GroundOffset = -Lowest;
		return Out;
	}

	AStaticMeshActor* SpawnMeshActor(UWorld* World, UStaticMesh* Mesh, const FTransform& Transform, const FString& Label)
	{
		if (!Mesh) return nullptr;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->SetActorLabel(Label);
		return Actor;
	}

	void SpawnVehicle(UWorld* World, const FVehicleAssets& Vehicle, const FBRRStart& Start, const FString& Label)
	{
		const FTransform Transform(FRotator(0, Start.Yaw, 0), Start.Location + FVector(0, 0, Vehicle.GroundOffset));
		AStaticMeshActor* Actor = SpawnMeshActor(World, Vehicle.Body, Transform, Label);
		if (!Actor) return;
		// A static preview of the grid; the race removes these and spawns the real cars.
		Actor->Tags.Add(TEXT("BRRPreview"));
		for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
		{
			UStaticMeshComponent* Wheel = NewObject<UStaticMeshComponent>(Actor, *FString::Printf(TEXT("Wheel%d"), I));
			Wheel->SetStaticMesh(Vehicle.Wheels[I].Key);
			Wheel->SetupAttachment(Actor->GetRootComponent());
			Wheel->SetRelativeLocation(Vehicle.Wheels[I].Value);
			Actor->AddInstanceComponent(Wheel);
			Wheel->RegisterComponent();
		}
	}
}

TArray<FString> BRRImport::FindLevels()
{
	TArray<FString> Levels;
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty()) return Levels;
	IFileManager::Get().FindFiles(Levels, *FPaths::Combine(DataDir, TEXT("*.LEV")), true, false);
	for (FString& L : Levels) L = FPaths::GetBaseFilename(L).ToUpper();
	Levels.Sort();
	return Levels;
}

bool BRRImport::ImportLevel(const FBRRImportOptions& Options, FString& OutMapPackage, FString& OutError)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		OutError = TEXT("Big Red Racing data folder not found: set BRR_DATA_DIR or [BRR] DataDir in DefaultGame.ini");
		return false;
	}
	const FString Level = Options.Level.ToUpper();

	TArray<FString> Vehicles = Options.Vehicles;
	if (Vehicles.IsEmpty())
	{
		FBRRWorld Probe;
		if (!FBRRWorld::Load(DataDir, Level, {}, Options.Colour, Probe, OutError)) return false;
		Vehicles = Probe.CarList;
	}

	FBRRWorld World;
	if (!FBRRWorld::Load(DataDir, Level, Vehicles, Options.Colour, World, OutError)) return false;
	UE_LOG(LogBRRImport, Display, TEXT("%s: %d terrain + %d decor triangles, %d atlases, vehicles: %s"), *Level,
		World.Terrain.NumTriangles(), World.Decor.NumTriangles(), World.Atlases.Num(), *FString::Join(Vehicles, TEXT(", ")));

	FLevelAssets Assets;
	Assets.World = &World;
	Assets.Dir = Options.DestRoot / Level;
	Assets.Prefix = Level;
	Assets.Masters = MakeMasters(Options.DestRoot);
	for (const FBRRMeshSection& Section : World.Terrain.Sections)
	{
		if (Section.Atlas != INDEX_NONE) Assets.TerrainAtlases.Add(Section.Atlas);
	}

	UStaticMesh* Terrain = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Terrain"), *Level), World.Terrain, true);
	UStaticMesh* Decor = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Decor"), *Level), World.Decor, true);
	TArray<FVehicleAssets> VehicleAssets;
	for (const FBRRVehicleMesh& Vehicle : World.Vehicles) VehicleAssets.Add(MakeVehicle(Assets, Vehicle));

	// Team colours: the original ships six repaints of every body (<BASE>1..6.3DW), the same
	// geometry naming a different palette block (ORR scene/vehicle_render.h). SM_<V>_Body_C<n> is
	// repaint n; the race picks each racer's (ABRRRaceGameMode::LoadVehicleMeshes).
	for (int32 Colour = 1; Colour <= 6; ++Colour)
	{
		FBRRWorld Painted;
		if (!FBRRWorld::Load(DataDir, Level, Vehicles, Colour, Painted, OutError)) return false;
		for (const FBRRVehicleMesh& Vehicle : Painted.Vehicles)
		{
			MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body_C%d"), *Vehicle.Name, Colour), Vehicle.Body, false, ESurface::Paint);
		}
	}

	// The map.
	OutMapPackage = Assets.Dir / FString::Printf(TEXT("L_%s"), *Level);
	UPackage* MapPackage = MakePackage(OutMapPackage);
	UWorld* Map = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FPackageName::GetLongPackageAssetName(OutMapPackage)), MapPackage);
	Map->SetFlags(AssetFlags);

	SpawnMeshActor(Map, Terrain, FTransform::Identity, TEXT("Terrain"));
	SpawnMeshActor(Map, Decor, FTransform::Identity, TEXT("Decor"));

	// The destructible decor is built by the race (ABRRWrecks) from the level data. Here: every
	// material its flipbooks read, and a preview of the standing pieces for the editor, which the
	// race removes like the grid preview cars.
	FBRRMesh WreckPreview;
	for (const FBRRWreck& W : World.Wrecks)
	{
		WreckPreview.Sections.Append(W.Intact.Sections);
		for (const FBRRMesh& Step : W.Steps)
		{
			for (const FBRRMeshSection& Section : Step.Sections) Assets.MaterialFor(Section.Atlas, Section.bTwoSided);
		}
	}
	// The `anim` decor is the race's too (ABRRAnimDecor); the editor gets each slot's first pose.
	FBRRMesh AnimPreview;
	for (const TArray<FBRRMesh>& Slot : World.AnimSteps)
	{
		for (int32 K = 0; K < Slot.Num(); ++K)
		{
			if (K == 0) AnimPreview.Sections.Append(Slot[K].Sections);
			for (const FBRRMeshSection& Section : Slot[K].Sections) Assets.MaterialFor(Section.Atlas, Section.bTwoSided);
		}
	}
	if (AStaticMeshActor* Preview = SpawnMeshActor(Map, MakeMesh(Assets, FString::Printf(TEXT("SM_%s_AnimPreview"), *Level), AnimPreview, false),
		FTransform::Identity, TEXT("AnimPreview")))
	{
		Preview->Tags.Add(TEXT("BRRPreview"));
		Preview->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}

	// Billboards are built by the race too (ABRRSprites), drawn two-sided.
	for (const FBRRSprite& Sprite : World.Sprites)
	{
		for (const FBRRSprite::FFrame& Frame : Sprite.Frames)
		{
			if (Frame.bOn) Assets.MaterialFor(Frame.Atlas, true);
		}
	}
	if (AStaticMeshActor* Preview = SpawnMeshActor(Map, MakeMesh(Assets, FString::Printf(TEXT("SM_%s_WreckPreview"), *Level), WreckPreview, false),
		FTransform::Identity, TEXT("WreckPreview")))
	{
		Preview->Tags.Add(TEXT("BRRPreview"));
		Preview->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}

	// The start gantry, one actor per countdown state; the race shows one at a time. Red, the
	// state the original shows before the start, is the only one visible and solid in the map.
	static const TCHAR* LightStates[] = { TEXT("Red"), TEXT("RedAmber"), TEXT("Amber"), TEXT("Green") };
	for (int32 K = 0; K < World.StartLights.Num() && K < UE_ARRAY_COUNT(LightStates); ++K)
	{
		UStaticMesh* Mesh = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_StartLights_%s"), *Level, LightStates[K]), World.StartLights[K], true);
		AStaticMeshActor* Lights = SpawnMeshActor(Map, Mesh, FTransform::Identity, FString::Printf(TEXT("StartLights_%s"), LightStates[K]));
		if (!Lights) continue;
		Lights->Tags.Add(*FString::Printf(TEXT("BRRStartLight%d"), K));
		if (K > 0)
		{
			UStaticMeshComponent* Comp = Lights->GetStaticMeshComponent();
			Comp->SetVisibility(false);
			Comp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		}
	}

	// The level's 256 colours as a 256x1 strip: the reference every pixel on screen should come from
	// (Tools/palette_check.py).
	if (World.Palette.Num() == 256)
	{
		FBRRImage Strip;
		Strip.Width = 256;
		Strip.Height = 1;
		Strip.Pixels = World.Palette;
		MakeTexture(Assets.Dir / TEXT("Textures") / FString::Printf(TEXT("T_%s_Palette"), *Level), Strip);
	}

	// The classic look: the original's sky, and palette colours shown exactly.
	if (World.SkyStrip.Num() == 18)
	{
		FBRRImage Strip;
		Strip.Width = 18;
		Strip.Height = 1;
		Strip.Pixels = World.SkyStrip;
		UTexture2D* SkyTexture = MakeTexture(Assets.Dir / TEXT("Textures") / FString::Printf(TEXT("T_%s_Sky"), *Level), Strip);
		UMaterialInstanceConstant* SkyMaterial = NewAsset<UMaterialInstanceConstant>(Assets.Dir / TEXT("Materials") / FString::Printf(TEXT("MI_%s_Sky"), *Level));
		SkyMaterial->SetParentEditorOnly(Assets.Masters.Sky);
		SkyMaterial->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Sky")), SkyTexture);
		SkyMaterial->PostEditChange();
		SaveAsset(SkyMaterial);

		const FVector Centre(World.Terrain.GetBounds().GetCenter());
		AStaticMeshActor* Sky = SpawnMeshActor(Map, LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")),
			FTransform(FRotator::ZeroRotator, FVector(Centre.X, Centre.Y, 0), FVector(50000.0)), TEXT("Sky"));
		UStaticMeshComponent* SkyMesh = Sky->GetStaticMeshComponent();
		SkyMesh->SetMaterial(0, SkyMaterial);
		SkyMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName); // a profile survives loading; a bare flag does not
		SkyMesh->SetCastShadow(false);
		Sky->Tags.Add(TEXT("BRRClassicSky")); // the remastered look has a real sky instead
	}
	{
		APostProcessVolume* Look = Map->SpawnActor<APostProcessVolume>();
		Look->SetActorLabel(TEXT("ClassicLook"));
		Look->Tags.Add(TEXT("BRRClassicLook")); // weighted out by the remastered look
		Look->bUnbound = true;
		FPostProcessSettings& P = Look->Settings;
		P.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, Assets.Masters.Display));
		P.bOverride_AutoExposureMethod = true;
		P.AutoExposureMethod = AEM_Manual;
		P.bOverride_AutoExposureBias = true;
		P.AutoExposureBias = 0.f;
		P.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		P.AutoExposureApplyPhysicalCameraExposure = false;
		P.bOverride_BloomIntensity = true;
		P.BloomIntensity = 0.f;
		P.bOverride_MotionBlurAmount = true;
		P.MotionBlurAmount = 0.f;
		P.bOverride_VignetteIntensity = true;
		P.VignetteIntensity = 0.f;
		P.bOverride_LensFlareIntensity = true;
		P.LensFlareIntensity = 0.f;
	}
	for (int32 Slot = 0; Slot < World.Grid.Num() && !VehicleAssets.IsEmpty(); ++Slot)
	{
		const int32 V = Slot % VehicleAssets.Num();
		SpawnVehicle(Map, VehicleAssets[V], World.Grid[Slot], FString::Printf(TEXT("Grid%d_%s"), Slot, *World.Vehicles[V].Name));
	}
	if (!World.Grid.IsEmpty())
	{
		// Ahead of pole position, looking back down the grid.
		const FBRRStart& Pole = World.Grid[0];
		const FVector Ahead = FRotator(0, Pole.Yaw, 0).Vector();
		const FVector Location = Pole.Location + Ahead * 2500.0 + FVector(0, 0, 500);
		APlayerStart* Start = Map->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FTransform(FRotator(0, Pole.Yaw + 180.0, 0), Location));
		Start->SetActorLabel(TEXT("PlayerStart"));
	}

	const bool bSaved = SaveAsset(Map);
	Map->DestroyWorld(false);
	Map->RemoveFromRoot();
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("could not save %s"), *OutMapPackage);
		return false;
	}
	UE_LOG(LogBRRImport, Display, TEXT("%s imported to %s (%d textures, %d materials)"), *Level, *OutMapPackage,
		Assets.Textures.Num(), Assets.Materials.Num());
	return true;
}
