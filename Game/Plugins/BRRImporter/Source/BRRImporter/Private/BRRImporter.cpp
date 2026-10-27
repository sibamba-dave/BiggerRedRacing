#include "BRRImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BRRArchive.h"
#include "BRRWorld.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "BRRRefine.h"
#include "BRRTrees.h"
#include "BRRProps.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2DArray.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionShadingModel.h"
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
	UTexture2D* MakeTexture(const FString& PackageName, const FBRRImage& Source, bool bMips = false)
	{
		FBRRImage Image = Source;
		if (bMips)
		{
			// The see-through texels take the colour of the art beside them (their alpha stays 0):
			// left black, the filtering and the mips averaged them into the sprites' edges, and a
			// small sprite seen from afar - spray, dust, a tree - came out dark. The classic look
			// masks them out whatever their colour.
			const int32 W = Image.Width, H = Image.Height;
			TArray<bool> Known;
			Known.SetNumUninitialized(Image.Pixels.Num());
			for (int32 I = 0; I < Image.Pixels.Num(); ++I) Known[I] = Image.Pixels[I].A >= 128;
			for (int32 Pass = 0; Pass < 16; ++Pass)
			{
				TArray<int32> Filled;
				for (int32 Y = 0; Y < H; ++Y)
				{
					for (int32 X = 0; X < W; ++X)
					{
						const int32 I = Y * W + X;
						if (Known[I]) continue;
						int32 R = 0, G = 0, B = 0, N = 0;
						for (int32 DY = -1; DY <= 1; ++DY)
						{
							for (int32 DX = -1; DX <= 1; ++DX)
							{
								const int32 NX = X + DX, NY = Y + DY;
								if (NX < 0 || NY < 0 || NX >= W || NY >= H || !Known[NY * W + NX]) continue;
								const FColor& C = Image.Pixels[NY * W + NX];
								R += C.R, G += C.G, B += C.B, ++N;
							}
						}
						if (N == 0) continue;
						Image.Pixels[I] = FColor(uint8(R / N), uint8(G / N), uint8(B / N), Image.Pixels[I].A);
						Filled.Add(I);
					}
				}
				if (Filled.IsEmpty()) break;
				for (int32 I : Filled) Known[I] = true;
			}
		}
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
		// The pages are 1024x768: without this they get no mips at all (and never stream), so far
		// terrain shimmered. Stretch, not pad: the materials sample 0..1.
		Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::StretchToPowerOfTwo;
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
		UTexture2DArray* Ground = nullptr; // the ground materials' detail, a slice per EBRRGround after None
		UMaterial* Scatter = nullptr;      // the scattered ground detail: the instance's colour, lit (remastered only)
		UMaterial* TexturedHD = nullptr;   // Textured, tessellated and displaced: the refined terrain's
		UMaterial* Water = nullptr;        // the remastered look's open water (single layer water)
		UMaterial* Effect = nullptr;       // the race's spray, dust and sparks (ABRRParticles): unlit in both looks
		UMaterial* Foam = nullptr;         // the boats' wakes and splashes (ABRRWake, remastered only)
		UMaterial* Haze = nullptr;         // the dust hanging over dirt (ABRRDust, remastered only)
	};

	// The ground materials Tools/fetch_ground.py packs (normal X/Y, roughness, brightness), one
	// slice each, in EBRRGround's order. Without them, one neutral slice: no detail, the import
	// otherwise the same.
	UTexture2DArray* MakeGroundArray(const FString& PackageName)
	{
		static const TCHAR* Kinds[] = { TEXT("dirt"), TEXT("grass"), TEXT("sand"), TEXT("snow"), TEXT("rock"), TEXT("asphalt"), TEXT("regolith") };
		static_assert(UE_ARRAY_COUNT(Kinds) == int32(EBRRGround::Count) - 1, "a slice per ground kind");
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../Tools/ground"));
		TArray<uint8> Data;
		int32 Size = 0, Slices = 0;
		for (const TCHAR* Kind : Kinds)
		{
			FImage Image;
			if (!FImageUtils::LoadImage(*(Dir / Kind + TEXT(".png")), Image) || (Size && (Image.SizeX != Size || Image.SizeY != Size)))
			{
				UE_LOG(LogBRRImport, Warning, TEXT("No ground material %s in %s (Tools/fetch_ground.py): the remastered ground has no detail"), Kind, *Dir);
				Slices = 0;
				break;
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace);
			Size = Image.SizeX;
			Data.Append(Image.RawData);
			++Slices;
		}
		if (Slices == 0)
		{
			Size = 4;
			Slices = 1;
			Data.Reset();
			for (int32 I = 0; I < 16; ++I) Data.Append({ 191, 128, 128, 128 }); // BGRA: flat, rough 0.75, average
		}
		UTexture2DArray* Texture = NewAsset<UTexture2DArray>(PackageName);
		Texture->Source.Init(Size, Size, Slices, 1, TSF_BGRA8, Data.GetData());
		Texture->SRGB = false;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->CompressionSettings = TC_BC7;
		Texture->PostEditChange();
		SaveAsset(Texture);
		return Texture;
	}

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

	// The remastered ground's detail (EBRRGround at the triangle's three corners in UV1.xy and UV2.x,
	// the corners' weights in UV3, by MakeMesh: the terrain only): its CC0 materials laid in the world
	// every 3 m, projected along the face's main axis, blended from corner to corner by weight and
	// height (one kind's pebbles and clumps stand out of the next before it gives way), modulating the
	// original's colour by its brightness and adding its relief and roughness to the page's own;
	// fading out by 160 m, where its tiling would show. Returns the colour (float3) and the
	// detail (float4) to hand ConnectLook in place of the ones given.
	TPair<UMaterialExpression*, UMaterialExpression*> AddGround(UMaterial* Mat, UMaterialParameterCollection* Collection, UTexture2DArray* Ground,
		UMaterialExpression* Colour, int32 ColourOutput, UMaterialExpression* Detail, bool bDisplace = false)
	{
		auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
		auto Coordinate = [&](int32 Index, int32 Y)
		{
			auto* C = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -1600, Y));
			C->CoordinateIndex = Index;
			return C;
		};
		UMaterialExpression* Kinds01 = Coordinate(1, 1400);
		UMaterialExpression* Kinds2 = Coordinate(2, 1450);
		UMaterialExpression* Weights = Coordinate(3, 1500);
		UMaterialExpression* Position = Make(UMaterialExpressionWorldPosition::StaticClass(), -1400, 1600);
		UMaterialExpression* Facing = Make(UMaterialExpressionVertexNormalWS::StaticClass(), -1400, 1700);
		UMaterialExpression* Depth = Make(UMaterialExpressionPixelDepth::StaticClass(), -1400, 1800);
		UMaterialExpression* Remaster = MakeCollectionValue(Mat, Collection, TEXT("Remaster"), -1400, 1900);
		// A sample per corner, of its kind's slice (a kind-less corner reads the first, unused).
		TArray<TTuple<FName, UMaterialExpression*, int32>> BlendInputs = { MakeTuple(FName(TEXT("A")), Kinds01, 0),
			MakeTuple(FName(TEXT("B")), Kinds2, 0), MakeTuple(FName(TEXT("W")), Weights, 0) };
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			UMaterialExpressionCustom* Coord = MakeCustomOutputs(Mat, FString::Printf(TEXT(
				"float3 a = abs(N);\n"
				"float2 uv = a.z >= max(a.x, a.y) ? P.xy : a.x >= a.y ? P.yz : P.xz;\n"
				"float k = %s;\n"
				"return float3(uv / 300.0, max(k - 1, 0));"), Corner == 0 ? TEXT("A.x") : Corner == 1 ? TEXT("A.y") : TEXT("B.x")), CMOT_Float3,
				{ MakeTuple(FName(TEXT("P")), Position, 0), MakeTuple(FName(TEXT("N")), Facing, 0), MakeTuple(FName(TEXT("A")), Kinds01, 0),
				  MakeTuple(FName(TEXT("B")), Kinds2, 0) }, 1300 + Corner * 100);
			auto* Sample = Cast<UMaterialExpressionTextureSampleParameter2DArray>(Make(UMaterialExpressionTextureSampleParameter2DArray::StaticClass(), -1100, 1300 + Corner * 100));
			Sample->ParameterName = TEXT("Ground");
			Sample->Texture = Ground;
			Sample->SamplerType = SAMPLERTYPE_LinearColor;
			Sample->SamplerSource = SSM_Wrap_WorldGroupSettings;
			Sample->Coordinates.Connect(0, Coord);
			BlendInputs.Add(MakeTuple(FName(*FString::Printf(TEXT("T%d"), Corner)), (UMaterialExpression*)Sample, 5));
		}
		// Kind-less corners are neutral (flat, average roughness and brightness): the detail fades
		// out towards them. Weighted by how near the corner and how high its material stands.
		UMaterialExpressionCustom* Sample = MakeCustomOutputs(Mat, TEXT(
			"const float4 n = float4(0.5, 0.5, 0.75, 0.5);\n"
			"float4 t0 = A.x > 0.5 ? T0 : n, t1 = A.y > 0.5 ? T1 : n, t2 = B.x > 0.5 ? T2 : n;\n"
			"float3 b = float3(W.x, W.y, saturate(1 - W.x - W.y));\n"
			"float3 h = b + 0.6 * float3(t0.a, t1.a, t2.a);\n"
			"float3 w = max(h - (max(h.x, max(h.y, h.z)) - 0.2), 0);\n"
			"w /= max(w.x + w.y + w.z, 1e-4);\n"
			"return t0 * w.x + t1 * w.y + t2 * w.z;"), CMOT_Float4, BlendInputs, 1600);
		// The sample's normal (DirectX: X along u, Y along v) into the world, by the projection's axes.
		UMaterialExpressionCustom* World = MakeCustomOutputs(Mat, TEXT(
			"float2 xy = T.rg * 2 - 1;\n"
			"float3 t = float3(xy, sqrt(saturate(1 - dot(xy, xy))));\n"
			"float3 a = abs(N);\n"
			"if (a.z >= max(a.x, a.y)) return float3(t.x, t.y, t.z * sign(N.z));\n"
			"if (a.x >= a.y) return float3(t.z * sign(N.x), t.x, t.y);\n"
			"return float3(t.x, t.z * sign(N.y), t.y);"), CMOT_Float3,
			{ MakeTuple(FName(TEXT("T")), (UMaterialExpression*)Sample, 0), MakeTuple(FName(TEXT("N")), Facing, 0) }, 1600);
		auto* ToTangent = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -500, 1600));
		ToTangent->Input.Connect(0, World);
		ToTangent->TransformSourceType = TRANSFORMSOURCE_World;
		ToTangent->TransformType = TRANSFORM_Tangent;
		UMaterialExpression* Wet = MakeCollectionValue(Mat, Collection, TEXT("Wetness"), -1400, 2000);
		const TCHAR* On = TEXT("float on = R > 0.5 ? saturate(1 - (Z - 4000) / 12000) : 0;\n");
		TArray<TTuple<FName, UMaterialExpression*, int32>> Common = {
			MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("Z")), Depth, 0), MakeTuple(FName(TEXT("T")), (UMaterialExpression*)Sample, 0) };
		TArray<TTuple<FName, UMaterialExpression*, int32>> ColourInputs = Common;
		ColourInputs.Add(MakeTuple(FName(TEXT("C")), Colour, ColourOutput));
		ColourInputs.Add(MakeTuple(FName(TEXT("Wet")), Wet, 0));
		// Rain-soaked ground reads darker; the darkening rides `on`, so the classic look (R = 0)
		// never sees it.
		UMaterialExpressionCustom* NewColour = MakeCustomOutputs(Mat, FString(On) +
			TEXT("return C.rgb * lerp(1, T.a * 2, on * 0.55) * lerp(1.0, 0.62, Wet * on);"), CMOT_Float3, ColourInputs, 1700);
		TArray<TTuple<FName, UMaterialExpression*, int32>> DetailInputs = Common;
		DetailInputs.Add(MakeTuple(FName(TEXT("D")), Detail, 0));
		DetailInputs.Add(MakeTuple(FName(TEXT("G")), (UMaterialExpression*)ToTangent, 0));
		UMaterialExpressionCustom* NewDetail = MakeCustomOutputs(Mat, FString(On) + TEXT(
			"float3 g = normalize(lerp(float3(0, 0, 1), normalize(G), on * 0.8));\n"
			"float3 n = normalize(float3(D.xy + g.xy, D.z * g.z));\n"
			"return float4(n, D.w + on * (T.b - 0.75) * 0.8);"), CMOT_Float4, DetailInputs, 1800);
		if (bDisplace)
		{
			// Nanite tessellation's displacement (0.5 is none): the material's brightness as its
			// height, the pebbles and clods standing up out of the ground.
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustomOutputs(Mat, TEXT("return 0.5 + (R > 0.5 ? (T.a - 0.5) : 0);"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("T")), (UMaterialExpression*)Sample, 0) }, 1900),
				TEXT(""), MP_Displacement);
		}
		return { NewColour, NewDetail };
	}

	// The ripples: four travelling waves, a few metres long, summed into a world-space slope and
	// handed to the material in tangent space (the atlas tiles turn and mirror from cell to cell,
	// so the waves are laid out in the world, not in UV).
	UMaterialExpression* MakeRipples(UMaterial* Mat)
	{
		auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
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
		return ToTangent;
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
		UMaterialExpression* Wet = MakeCollectionValue(Mat, Collection, TEXT("Wetness"), -800, 1200);
		auto Part = [&](const TCHAR* Code, ECustomMaterialOutputType Type, int32 Y)
		{
			return MakeCustomOutputs(Mat, FString::Printf(TEXT("int k = (int)K;\n%s"), Code), Type,
				{ MakeTuple(FName(TEXT("C")), Colour, ColourOutput), MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("K")), (UMaterialExpression*)Kind, 0),
				  MakeTuple(FName(TEXT("Rough")), Rough, 0), MakeTuple(FName(TEXT("Shine")), Shine, 0), MakeTuple(FName(TEXT("D")), Detail, 0),
				  MakeTuple(FName(TEXT("Wet")), Wet, 0) }, Y);
		};
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return C.rgb * (1 - R) + (k == 3 ? C.rgb * 16.0 : 0);"), CMOT_Float3, 0), TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return C.rgb * R * (k == 1 ? 0.35 : k == 3 ? 0.02 : 1.0);"), CMOT_Float3, 100), TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT("return R * (k == 1 || k == 2 ? 0.5 : Shine);"), CMOT_Float1, 200), TEXT(""), MP_Specular);
		// Rain polishes what it soaks (Wet rides R: the classic look stays matte-unlit).
		UMaterialEditingLibrary::ConnectMaterialProperty(Part(TEXT(
			"float r = k == 1 ? 0.03 : k == 2 ? 0.1 : k == 3 ? 0.55 : saturate(Rough + (D.w - 0.5));\n"
			"return lerp(r, min(r, 0.25), Wet * R);"), CMOT_Float1, 300), TEXT(""), MP_Roughness);

		UMaterialExpression* ToTangent = MakeRipples(Mat);
		UMaterialExpressionCustom* Normal = MakeCustomOutputs(Mat, TEXT("return K == 1 ? normalize(N) : R > 0.5 ? normalize(D.xyz) : float3(0, 0, 1);"), CMOT_Float3,
			{ MakeTuple(FName(TEXT("N")), (UMaterialExpression*)ToTangent, 0), MakeTuple(FName(TEXT("K")), (UMaterialExpression*)Kind, 0),
			  MakeTuple(FName(TEXT("R")), Remaster, 0), MakeTuple(FName(TEXT("D")), Detail, 0) }, 900);
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);

		// Vehicle paint rides a clear coat (per-pixel shading model): instances with ClearCoat 1
		// get the lacquered second lobe in the remastered look - toy-car gloss - while everything
		// else stays default lit, and the classic look's emissive-only output never reads a
		// shading model at all.
		// As the editor's dropdown does: the sentinel goes straight into the property (SetShadingModel
		// asserts on it), and the recompile's PostEditChange rebuilds the shading-model field from
		// the expressions.
		if (FByteProperty* ModelProp = FindFProperty<FByteProperty>(UMaterial::StaticClass(), TEXT("ShadingModel")))
			ModelProp->SetPropertyValue_InContainer(Mat, MSM_FromMaterialExpression);
		UMaterialExpression* Coat = Scalar(TEXT("ClearCoat"), 0.f, 1200);
		auto* Lit = Cast<UMaterialExpressionShadingModel>(Make(UMaterialExpressionShadingModel::StaticClass(), -500, 1300));
		Lit->ShadingModel = MSM_DefaultLit;
		auto* Lacquer = Cast<UMaterialExpressionShadingModel>(Make(UMaterialExpressionShadingModel::StaticClass(), -500, 1400));
		Lacquer->ShadingModel = MSM_ClearCoat;
		UMaterialExpressionCustom* Gate = MakeCustom(Mat, TEXT("return CC * R;"), CMOT_Float1,
			{ { TEXT("CC"), Coat }, { TEXT("R"), Remaster } });
		auto* Pick = Cast<UMaterialExpressionIf>(Make(UMaterialExpressionIf::StaticClass(), -300, 1350));
		Pick->A.Connect(0, Gate);
		Pick->ConstB = 0.5f;
		Pick->AGreaterThanB.Connect(0, Lacquer);
		Pick->ALessThanB.Connect(0, Lit);
		Pick->AEqualsB.Connect(0, Lit);
		UMaterialEditingLibrary::ConnectMaterialProperty(Pick, TEXT(""), MP_ShadingModel);
		UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return CC * R * 0.9;"), CMOT_Float1,
			{ { TEXT("CC"), Coat }, { TEXT("R"), Remaster } }), TEXT(""), MP_CustomData0);
		UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return 0.08;"), CMOT_Float1, {}), TEXT(""), MP_CustomData1);
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
			for (int32 R = 0; R < 6; ++R)
			{
				// A racer's world position each frame (w: 1 while racing, 0 unused): the crowd's
				// cheering reads these in the shader (M_BRR_Crowd), so no instance is ever touched.
				FCollectionVectorParameter Racer;
				Racer.ParameterName = FName(*FString::Printf(TEXT("Racer%d"), R));
				Racer.DefaultValue = FLinearColor(0, 0, 0, 0);
				Collection->VectorParameters.Add(Racer);
			}
			for (const TCHAR* Name : { TEXT("WeatherVelX"), TEXT("WeatherVelY"), TEXT("WeatherFall"), TEXT("Wetness") })
			{
				// The falling weather's drift and fall speeds (cm/s) and how soaked the ground
				// lies (0..1), set once per race (ABRRWeather / ABRRLook::SetMood): M_BRR_Rain and
				// M_BRR_Snow move every drop in the shader, and the ground blend darkens and
				// polishes by the wetness. All are remaster-gated where read.
				FCollectionScalarParameter P;
				P.ParameterName = Name;
				P.DefaultValue = 0.f;
				Collection->ScalarParameters.Add(P);
			}
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
		M.Ground = MakeGroundArray(Root / TEXT("Textures/T_BRR_Ground"));
		{
			// The scattered grass and stones (ABRRScatter): the ground's colour under each, from the
			// instance, shaded by the vertex colour (the grass darker at its root). Only the
			// remastered look shows them, so there is no classic path.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Scatter"));
			Mat->bUsedWithNanite = true;
			Mat->bUsedWithInstancedStaticMeshes = true;
			Mat->SetShadingModel(MSM_DefaultLit);
			Mat->TwoSided = true;
			UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVertexColor::StaticClass(), -700, 0);
			TArray<TPair<FName, UMaterialExpression*>> Inputs = { { TEXT("Srgb"), Colour } };
			for (int32 I = 0; I < 3; ++I)
			{
				auto* Data = Cast<UMaterialExpressionPerInstanceCustomData>(
					UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionPerInstanceCustomData::StaticClass(), -700, 150 + I * 100));
				Data->DataIndex = I;
				Data->ConstDefaultValue = 0.5f;
				Inputs.Add({ FName(*FString::Printf(TEXT("I%d"), I)), Data });
			}
			UMaterialExpressionCustom* Base = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"float3 shade = lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));\n"
				"return shade * float3(I0, I1, I2);"), CMOT_Float3, Inputs);
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
			auto Constant = [&](float Value, int32 Y)
			{
				auto* C = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionScalarParameter::StaticClass(), -300, Y));
				C->DefaultValue = Value;
				return C;
			};
			auto* Rough = Constant(0.8f, 300);
			Rough->ParameterName = TEXT("Roughness");
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
			auto* Spec = Constant(0.3f, 400);
			Spec->ParameterName = TEXT("Specular");
			UMaterialEditingLibrary::ConnectMaterialProperty(Spec, TEXT(""), MP_Specular);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Scatter = Mat;
		}
		{
			// The spectators (ABRRCrowd): the scatter shading plus the cheering, all in the shader.
			// Each instance bobs on its own phase (custom data 3-5) and jumps as a racer - the six
			// MPC Racer vectors, written once a frame - comes near, so after its build the crowd
			// costs the CPU nothing.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Crowd"));
			Mat->bUsedWithNanite = true;
			Mat->bUsedWithInstancedStaticMeshes = true;
			Mat->SetShadingModel(MSM_DefaultLit);
			Mat->TwoSided = true;
			auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
			UMaterialExpression* Colour = Make(UMaterialExpressionVertexColor::StaticClass(), -700, 0);
			TArray<TPair<FName, UMaterialExpression*>> Inputs = { { TEXT("Srgb"), Colour } };
			TArray<TPair<FName, UMaterialExpression*>> WaveIn;
			for (int32 I = 0; I < 6; ++I)
			{
				auto* Data = Cast<UMaterialExpressionPerInstanceCustomData>(Make(UMaterialExpressionPerInstanceCustomData::StaticClass(), -700, 150 + I * 100));
				Data->DataIndex = I;
				Data->ConstDefaultValue = I < 3 ? 0.5f : 0.f;
				(I < 3 ? Inputs : WaveIn).Add({ FName(*FString::Printf(TEXT("I%d"), I)), Data });
			}
			UMaterialExpressionCustom* Base = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"float3 shade = lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));\n"
				"return shade * float3(I0, I1, I2);"), CMOT_Float3, Inputs);
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
			WaveIn.Add({ TEXT("T"), Make(UMaterialExpressionTime::StaticClass(), -700, 800) });
			WaveIn.Add({ TEXT("WP"), Make(UMaterialExpressionWorldPosition::StaticClass(), -700, 900) });
			auto* Local = Cast<UMaterialExpressionLocalPosition>(Make(UMaterialExpressionLocalPosition::StaticClass(), -700, 1000));
			Local->LocalOrigin = ELocalPositionOrigin::Instance;
			Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
			WaveIn.Add({ TEXT("LP"), Local });
			for (int32 R = 0; R < 6; ++R)
				WaveIn.Add({ FName(*FString::Printf(TEXT("R%d"), R)),
					MakeCollectionValue(Mat, Collection, FName(*FString::Printf(TEXT("Racer%d"), R)), -700, 1100 + R * 100) });
			// I3 phase, I4 pace, I5 jump share (0 for the sitting variant). The lean pivots at the
			// feet, as the CPU crowd's roll did: its reach grows with the vertex's height.
			UMaterialExpressionCustom* Wave = MakeCustom(Mat, TEXT(
				"float t = T * I4 * 6.0 + I3;\n"
				"float near = 1e9;\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R0.xy), step(0.5, R0.w)));\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R1.xy), step(0.5, R1.w)));\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R2.xy), step(0.5, R2.w)));\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R3.xy), step(0.5, R3.w)));\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R4.xy), step(0.5, R4.w)));\n"
				"near = min(near, lerp(1e9, distance(WP.xy, R5.xy), step(0.5, R5.w)));\n"
				"float excite = 1.0 - smoothstep(2200.0, 4000.0, near);\n"
				"float jump = max(0.0, sin(t)) * 28.0 * excite * I5;\n"
				"float lean = sin(t * 0.37) * (2.0 + 6.0 * excite) * 0.0175 * max(LP.z, 0.0);\n"
				"return float3(0.0, lean, jump);"), CMOT_Float3, WaveIn);
			UMaterialEditingLibrary::ConnectMaterialProperty(Wave, TEXT(""), MP_WorldPositionOffset);
			auto* Rough = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -300, 300));
			Rough->DefaultValue = 0.8f;
			Rough->ParameterName = TEXT("Roughness");
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
		}
		{
			// The flags' cloth (ABRRCrowd): built once and waved in the shader along the vertex
			// normal (the cloth's side). UV0 runs along (U) and down (V) the cloth; UV1 carries the
			// flag's width and its phase.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Cloth"));
			Mat->SetShadingModel(MSM_DefaultLit);
			Mat->TwoSided = true;
			auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
			UMaterialExpression* Colour = Make(UMaterialExpressionVertexColor::StaticClass(), -700, 0);
			UMaterialExpressionCustom* Base = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));"), CMOT_Float3, { { TEXT("Srgb"), Colour } });
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
			auto* Along = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -700, 200));
			auto* Seed = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -700, 300));
			Seed->CoordinateIndex = 1;
			UMaterialExpressionCustom* Wave = MakeCustom(Mat, TEXT(
				"float wave = sin(UV.x * 7.0 - T * 6.0 + Seed.y) * Seed.x * 0.07 * UV.x\n"
				"           + sin(UV.x * 13.0 - T * 9.0) * Seed.x * 0.02 * UV.x;\n"
				"return N * wave;"), CMOT_Float3,
				{ { TEXT("UV"), Along }, { TEXT("Seed"), Seed }, { TEXT("N"), Make(UMaterialExpressionVertexNormalWS::StaticClass(), -700, 400) },
				  { TEXT("T"), Make(UMaterialExpressionTime::StaticClass(), -700, 500) } });
			UMaterialEditingLibrary::ConnectMaterialProperty(Wave, TEXT(""), MP_WorldPositionOffset);
			auto* Rough = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -300, 300));
			Rough->DefaultValue = 0.7f;
			Rough->ParameterName = TEXT("Roughness");
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
		}
		for (const bool bSnow : { false, true })
		{
			// The falling weather (ABRRWeather): a box of drops built once, all four corners of each
			// quad on the drop's point. The shader falls, drifts and wraps every drop in the box
			// (4000 x 4000 x 2200, as BRRWeather.cpp builds it) and spreads the corners - rain into
			// a streak stretched along its fall, snow into a small flake that wanders as it sinks.
			// UV0 is the corner, UV1 the drop's seed, the vertex colour the drop's own.
			UMaterial* Mat = NewAsset<UMaterial>(Root / (bSnow ? TEXT("Materials/M_BRR_Snow") : TEXT("Materials/M_BRR_Rain")));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Translucent;
			Mat->TwoSided = true;
			auto Make = [Mat](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, X, Y); };
			UMaterialExpression* Vertex = Make(UMaterialExpressionVertexColor::StaticClass(), -900, 0);
			UMaterialExpression* Corner = Make(UMaterialExpressionTextureCoordinate::StaticClass(), -900, 150);
			auto* Seed = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -900, 250));
			Seed->CoordinateIndex = 1;
			auto* Local = Cast<UMaterialExpressionLocalPosition>(Make(UMaterialExpressionLocalPosition::StaticClass(), -900, 350));
			Local->LocalOrigin = ELocalPositionOrigin::Primitive;
			Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
			auto Axis = [&](float X, float Y, int32 GraphY)
			{
				auto* Dir = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -1100, GraphY));
				Dir->Constant = FLinearColor(X, Y, 0);
				auto* World = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass(), -900, GraphY));
				World->Input.Connect(0, Dir);
				World->TransformSourceType = TRANSFORMSOURCE_View;
				World->TransformType = TRANSFORM_World;
				return World;
			};
			TArray<TPair<FName, UMaterialExpression*>> In = {
				{ TEXT("LP"), Local }, { TEXT("U"), Corner }, { TEXT("S"), Seed },
				{ TEXT("T"), Make(UMaterialExpressionTime::StaticClass(), -900, 450) },
				{ TEXT("VX"), MakeCollectionValue(Mat, Collection, TEXT("WeatherVelX"), -900, 550) },
				{ TEXT("VY"), MakeCollectionValue(Mat, Collection, TEXT("WeatherVelY"), -900, 650) },
				{ TEXT("F"), MakeCollectionValue(Mat, Collection, TEXT("WeatherFall"), -900, 750) },
				{ TEXT("R"), Axis(1, 0, 850) } };
			if (bSnow) In.Add({ TEXT("UP"), Axis(0, 1, 950) });
			UMaterialExpressionCustom* Wpo = MakeCustom(Mat, bSnow ? TEXT(
				"float3 p = LP;\n"
				"p.z = frac((p.z - T * F) / 2200.0) * 2200.0;\n"
				"p.x = frac((p.x + T * VX + sin(p.z * 0.01 + S.y) * 33.0) / 4000.0) * 4000.0;\n"
				"p.y = frac((p.y + T * VY + cos(p.z * 0.013 + S.y) * 33.0) / 4000.0) * 4000.0;\n"
				"float2 c = U.xy * 2.0 - 1.0;\n"
				"return (p - LP) + R * (c.x * 2.2) + UP * (c.y * 2.2);") : TEXT(
				"float3 p = LP;\n"
				"p.z = frac((p.z - T * F) / 2200.0) * 2200.0;\n"
				"p.x = frac((p.x + T * VX) / 4000.0) * 4000.0;\n"
				"p.y = frac((p.y + T * VY) / 4000.0) * 4000.0;\n"
				"float2 c = U.xy * 2.0 - 1.0;\n"
				"float3 along = normalize(float3(VX, VY, -max(F, 1.0))) * 45.0;\n"
				"return (p - LP) + R * (c.x * 0.8) + along * c.y;"), CMOT_Float3, In);
			UMaterialEditingLibrary::ConnectMaterialProperty(Wpo, TEXT(""), MP_WorldPositionOffset);
			UMaterialExpressionCustom* Opacity = MakeCustomOutputs(Mat, bSnow ? TEXT(
				"float2 c = U.xy * 2.0 - 1.0;\n"
				"return saturate(1 - length(c)) * A;") : TEXT(
				"float2 c = U.xy * 2.0 - 1.0;\n"
				"return saturate(1 - abs(c.x)) * saturate(1 - c.y * c.y) * A;"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("A")), Vertex, 4), MakeTuple(FName(TEXT("U")), Corner, 0) }, 0);
			UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return C.rgb * 1.5;"), CMOT_Float3, { { TEXT("C"), Vertex } }), TEXT(""), MP_EmissiveColor);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
		}
		for (const bool bHD : { false, true })
		{
			UMaterial* Mat = NewAsset<UMaterial>(Root / (bHD ? TEXT("Materials/M_BRR_TexturedHD") : TEXT("Materials/M_BRR_Textured")));
			Mat->bUsedWithNanite = true; // the terrain and the decor are Nanite (MakeMesh)
			if (bHD)
			{
				// The refined terrain only: Nanite tessellates it and the ground materials displace it,
				// a few centimetres either way.
				Mat->bEnableTessellation = true;
				Mat->DisplacementScaling.Magnitude = 6.f;
				Mat->DisplacementScaling.Center = 0.5f;
			}
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
			const TPair<UMaterialExpression*, UMaterialExpression*> Ground = AddGround(Mat, Collection, M.Ground, Colour, 0, Detail, bHD);
			ConnectLook(Mat, Collection, Ground.Key, 0, Ground.Value);
			auto* Mask = MakeCustomOutputs(Mat, TEXT("return C.a * D;"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("C")), (UMaterialExpression*)Colour, 0), MakeTuple(FName(TEXT("D")), MakeDrawRange(Mat, Collection), 0) }, 600);
			UMaterialEditingLibrary::ConnectMaterialProperty(Mask, TEXT(""), MP_OpacityMask);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			(bHD ? M.TexturedHD : M.Textured) = Mat;
		}
		{
			// The open water (BRR::SplitWater): UE's single layer water over the bed under it, so
			// it is clear at the banks and its own colour in the deep. Its colour is the level's
			// (WaterColour, the original's water art's), its surface the ripples.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Water"));
			Mat->SetShadingModel(MSM_SingleLayerWater);
			Mat->BlendMode = BLEND_Opaque;
			auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVectorParameter::StaticClass(), -800, 0));
			Tint->ParameterName = TEXT("WaterColour");
			Tint->DefaultValue = FLinearColor(0.02f, 0.08f, 0.12f);
			auto* Out = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass(), 0, 400));
			// Per metre: light scattered back in the water's colour, the red absorbed first.
			Out->ScatteringCoefficients.Connect(0, MakeCustom(Mat, TEXT("return C.rgb * 0.6;"), CMOT_Float3, { { TEXT("C"), Tint } }));
			Out->AbsorptionCoefficients.Connect(0, MakeCustom(Mat, TEXT("return float3(0.45, 0.09, 0.06) + (1 - C.rgb) * 0.05;"), CMOT_Float3, { { TEXT("C"), Tint } }));
			Out->PhaseG.Connect(0, MakeCustom(Mat, TEXT("return 0.1;"), CMOT_Float1, {}));
			UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT(""), MP_BaseColor);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return 0.04;"), CMOT_Float1, {}), TEXT(""), MP_Roughness);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return 0.5;"), CMOT_Float1, {}), TEXT(""), MP_Specular);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeRipples(Mat), TEXT(""), MP_Normal);
			{
				// The surface itself bobs: three slow trains summed into a vertical offset, swelling
				// as the rain stands on the water (MPC Wetness). The ripple normals above stay the
				// close-up detail; this is the body of the water moving.
				UMaterialExpression* WavePos = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionWorldPosition::StaticClass(), -1100, 1000);
				UMaterialExpression* WaveTime = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTime::StaticClass(), -1100, 1100);
				UMaterialExpression* Wet = MakeCollectionValue(Mat, Collection, TEXT("Wetness"), -1100, 1200);
				UMaterialExpressionCustom* Waves = MakeCustom(Mat, TEXT(
					"const float4 W[3] = { float4(0.8, 0.6, 1700.0, 0.55), float4(-0.5, 0.87, 1100.0, 0.8), float4(0.2, -0.98, 700.0, 1.1) };\n"
					"float h = 0;\n"
					"for (int i = 0; i < 3; i++) h += sin(dot(W[i].xy, P.xy) * 6.2831853 / W[i].z - T * W[i].w);\n"
					"return float3(0, 0, h * (4.0 + 6.0 * Wet));"), CMOT_Float3,
					{ { TEXT("P"), WavePos }, { TEXT("T"), WaveTime }, { TEXT("Wet"), Wet } });
				UMaterialEditingLibrary::ConnectMaterialProperty(Waves, TEXT(""), MP_WorldPositionOffset);
			}
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Water = Mat;
		}
		{
			// The ground effects: unlit in both looks. Classic as Textured's classic path, the palette
			// exact; remastered the smooth page at Brightness - about what a sunlit surface of that
			// colour gives (ABRRLook's 10 lux sun) - so a splash is not darkened by the water it
			// stands in, the shadow it is in, or the occlusion of the ground around it. Translucent, at 60%:
			// classic's alpha is the page's own, all or nothing, the palette still exact where it
			// shows; remastered's the filtered page's, a soft edge and a splash you see through.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Effect"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Translucent;
			Mat->TwoSided = true;
			UTexture2D* Default = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			auto* Classic = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, 0));
			Classic->ParameterName = TEXT("Atlas");
			Classic->Texture = Default;
			Classic->MipValueMode = TMVM_MipLevel;
			Classic->ConstMipValue = 0;
			auto* Smooth = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, 250));
			Smooth->ParameterName = TEXT("AtlasHD");
			Smooth->Texture = Default;
			Smooth->SamplerSource = SSM_Wrap_WorldGroupSettings;
			auto* Brightness = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionScalarParameter::StaticClass(), -800, 450));
			Brightness->ParameterName = TEXT("Brightness");
			Brightness->DefaultValue = 2.5f;
			UMaterialExpression* Remaster = MakeCollectionValue(Mat, Collection, TEXT("Remaster"), -800, 550);
			UMaterialExpressionCustom* Colour = MakeCustomOutputs(Mat, TEXT("return R > 0.5 ? float4(B.rgb * L, saturate(B.a * 1.3) * 0.6) : float4(A.rgb, A.a > 0.5);"), CMOT_Float4,
				{ MakeTuple(FName(TEXT("A")), (UMaterialExpression*)Classic, 5), MakeTuple(FName(TEXT("B")), (UMaterialExpression*)Smooth, 5),
				  MakeTuple(FName(TEXT("L")), (UMaterialExpression*)Brightness, 0), MakeTuple(FName(TEXT("R")), Remaster, 0) }, 500);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return C.rgb;"), CMOT_Float3, { { TEXT("C"), Colour } }), TEXT(""), MP_EmissiveColor);
			auto* Mask = MakeCustomOutputs(Mat, TEXT("return C.a * D;"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("C")), (UMaterialExpression*)Colour, 0), MakeTuple(FName(TEXT("D")), MakeDrawRange(Mat, Collection), 0) }, 600);
			UMaterialEditingLibrary::ConnectMaterialProperty(Mask, TEXT(""), MP_Opacity);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Effect = Mat;
		}
		{
			// The wakes (ABRRWake): white water, see-through, broken up by two octaves of noise that
			// drift with the strip (UV.x) and boil in place (time); soft at a strip's edges (UV.y);
			// as strong as the vertex alpha. Unlit at the effects' brightness (M_BRR_Effect).
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Foam"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Translucent;
			Mat->TwoSided = true;
			UMaterial* Base = Mat;
			auto Make = [Base](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Base, Class, X, Y); };
			UMaterialExpression* Vertex = Make(UMaterialExpressionVertexColor::StaticClass(), -900, 0);
			UMaterialExpression* Uv = Make(UMaterialExpressionTextureCoordinate::StaticClass(), -900, 150);
			UMaterialExpression* Where = Make(UMaterialExpressionWorldPosition::StaticClass(), -900, 300);
			UMaterialExpression* Time = Make(UMaterialExpressionTime::StaticClass(), -900, 450);
			const TCHAR* Noise = TEXT(
				"float2 p = P.xy / 90.0 + float2(U.x * 3.0, 0);\n"
				"float n = 0, a = 0.6;\n"
				"for (int o = 0; o < 2; o++)\n"
				"{\n"
				"  float2 i = floor(p), f = frac(p);\n"
				"  f = f * f * (3 - 2 * f);\n"
				"  float h00 = frac(sin(dot(i, float2(127.1, 311.7)) + T * 0.7) * 43758.5);\n"
				"  float h10 = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7)) + T * 0.7) * 43758.5);\n"
				"  float h01 = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7)) + T * 0.7) * 43758.5);\n"
				"  float h11 = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7)) + T * 0.7) * 43758.5);\n"
				"  n += a * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);\n"
				"  p *= 2.7; a *= 0.4;\n"
				"}\n"
				"float edge = sin(saturate(U.y) * 3.14159);\n"
				"return saturate((n - 0.25) * 1.8) * edge * C;");
			UMaterialExpressionCustom* Opacity = MakeCustomOutputs(Mat, Noise, CMOT_Float1,
				{ MakeTuple(FName(TEXT("C")), Vertex, 4), MakeTuple(FName(TEXT("U")), Uv, 0), MakeTuple(FName(TEXT("P")), Where, 0), MakeTuple(FName(TEXT("T")), Time, 0) }, 0);
			UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
			// A sunlit white (ABRRLook's 10 lux sun gives a white surface about 3, its sky a little more).
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return float3(0.92, 0.96, 0.95) * 4.0;"), CMOT_Float3, {}), TEXT(""), MP_EmissiveColor);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Foam = Mat;
		}
		{
			// The dust haze (ABRRDust): soft round cards in the vertex colour (linear) at a sunlit
			// brightness, as see-through as the vertex alpha, thinned out by a slowly boiling noise.
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Haze"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Translucent;
			Mat->TwoSided = true;
			UMaterial* Base = Mat;
			auto Make = [Base](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Base, Class, X, Y); };
			UMaterialExpression* Vertex = Make(UMaterialExpressionVertexColor::StaticClass(), -900, 0);
			UMaterialExpression* Uv = Make(UMaterialExpressionTextureCoordinate::StaticClass(), -900, 150);
			UMaterialExpression* Time = Make(UMaterialExpressionTime::StaticClass(), -900, 300);
			UMaterialExpressionCustom* Opacity = MakeCustomOutputs(Mat, TEXT(
				"float2 q = float2(frac(U.x), U.y) - 0.5;\n"
				"float r = saturate(1 - length(q) * 2);\n"
				"float2 p = q * 3.0 + floor(U.x) * 1.37 + T * 0.04;\n"
				"float n = 0, a = 0.6;\n"
				"for (int o = 0; o < 2; o++)\n"
				"{\n"
				"  float2 i = floor(p), f = frac(p);\n"
				"  f = f * f * (3 - 2 * f);\n"
				"  float h00 = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5);\n"
				"  float h10 = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5);\n"
				"  float h01 = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5);\n"
				"  float h11 = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5);\n"
				"  n += a * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);\n"
				"  p *= 2.3; a *= 0.45;\n"
				"}\n"
				"return r * (0.5 + 0.7 * n) * A;"), CMOT_Float1,
				{ MakeTuple(FName(TEXT("A")), Vertex, 4), MakeTuple(FName(TEXT("U")), Uv, 0), MakeTuple(FName(TEXT("T")), Time, 0) }, 0);
			UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeCustom(Mat, TEXT("return C.rgb * 3.0;"), CMOT_Float3, { { TEXT("C"), Vertex } }), TEXT(""), MP_EmissiveColor);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Haze = Mat;
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
			Mat->bUsedWithNanite = true; // the terrain and the decor are Nanite (MakeMesh)
			Mat->SetShadingModel(MSM_DefaultLit);
			UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVertexColor::StaticClass(), -500, 0);
			UMaterialExpressionCustom* Decode = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));"), CMOT_Float3, { { TEXT("Srgb"), Colour } });
			auto* Detail = Cast<UMaterialExpressionConstant4Vector>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionConstant4Vector::StaticClass(), -800, 1300));
			Detail->Constant = FLinearColor(0, 0, 1, 0.5f); // flat colours have no relief
			const TPair<UMaterialExpression*, UMaterialExpression*> Ground = AddGround(Mat, Collection, M.Ground, Decode, 0, Detail);
			ConnectLook(Mat, Collection, Ground.Key, 0, Ground.Value);
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
	// Effect: the race's ground effects (M_BRR_Effect).
	enum class ESurface : uint8 { Ground, Paint, Rubber, Refined, Effect };

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
				Surface == ESurface::Paint ? TEXT("_Paint") : Surface == ESurface::Rubber ? TEXT("_Rubber") : Surface == ESurface::Refined ? TEXT("_HD") : Surface == ESurface::Effect ? TEXT("_FX") : TEXT(""));
			UMaterialInstanceConstant* MI = NewAsset<UMaterialInstanceConstant>(Dir / TEXT("Materials") / Name);
			MI->SetParentEditorOnly(Atlas == INDEX_NONE ? Masters.Flat : Surface == ESurface::Refined ? Masters.TexturedHD : Surface == ESurface::Effect ? Masters.Effect : Masters.Textured);
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
				MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ClearCoat")), 1.f);
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
		ESurface Surface = ESurface::Ground, bool bNanite = false)
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
		UVs.SetNumChannels(4); // 1-3: the terrain's ground kinds at the triangle's corners, where in it (AddGround)

		const int32 Verts = Source.NumTriangles() * 3;
		Desc.ReserveNewVertices(Verts);
		Desc.ReserveNewVertexInstances(Verts);
		Desc.ReserveNewTriangles(Source.NumTriangles());

		UStaticMesh* Mesh = NewAsset<UStaticMesh>(Assets.Dir / TEXT("Meshes") / Name);
		int32 GroundCounts[int32(EBRRGround::Count)] = {};

		// The terrain's ground kinds: each triangle's from its land and art, then each corner's -
		// what most of the ground around it is - rounded off twice (a triangle takes what two of
		// its corners are, the corners vote again), so the material blends across a triangle from
		// corner to corner and the kinds meet along curves rather than the triangles' steps.
		TArray<uint8> TriangleGround;
		TArray<int32> CornerPoint;
		TArray<uint8> PointGround;
		{
			TMap<FIntVector, int32> Points;
			TArray<float> TriangleArea;
			for (const FBRRMeshSection& Section : Source.Sections)
			{
				for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
				{
					EBRRGround Ground = EBRRGround::None;
					if (const uint8 Land = Section.Vertices[T].Land; Land != 0xFF)
					{
						const bool bPage = Assets.World->Atlases.IsValidIndex(Section.Atlas);
						const FColor Average = bPage ? FBRRWorld::AverageTexels(Assets.World->Atlases[Section.Atlas], Section.Vertices[T].UV,
							Section.Vertices[T + 1].UV, Section.Vertices[T + 2].UV) : Section.Vertices[T].Color;
						Ground = Assets.World->GroundOf(Land, Average);
						++GroundCounts[int32(Ground)];
					}
					TriangleGround.Add(uint8(Ground));
					const FVector3f A = Section.Vertices[T].Position;
					TriangleArea.Add(((Section.Vertices[T + 1].Position - A) ^ (Section.Vertices[T + 2].Position - A)).Size() * 0.5f);
					for (int32 K = 0; K < 3; ++K)
					{
						const FVector3f& P = Section.Vertices[T + K].Position;
						const FIntVector Key(FMath::RoundToInt(P.X), FMath::RoundToInt(P.Y), FMath::RoundToInt(P.Z));
						CornerPoint.Add(Points.FindOrAdd(Key, Points.Num()));
					}
				}
			}
			constexpr int32 Kinds = int32(EBRRGround::Count);
			TArray<float> Votes;
			auto Vote = [&]()
			{
				Votes.Init(0.f, Points.Num() * Kinds);
				for (int32 T = 0; T < TriangleGround.Num(); ++T)
				{
					for (int32 K = 0; K < 3; ++K) Votes[CornerPoint[T * 3 + K] * Kinds + TriangleGround[T]] += TriangleArea[T];
				}
				PointGround.SetNumUninitialized(Points.Num());
				for (int32 P = 0; P < Points.Num(); ++P)
				{
					int32 Best = 0;
					for (int32 G = 1; G < Kinds; ++G) if (Votes[P * Kinds + G] > Votes[P * Kinds + Best]) Best = G;
					PointGround[P] = uint8(Best);
				}
			};
			Vote();
			for (int32 Round = 0; Round < 2; ++Round)
			{
				for (int32 T = 0; T < TriangleGround.Num(); ++T)
				{
					const uint8 A = PointGround[CornerPoint[T * 3]], B = PointGround[CornerPoint[T * 3 + 1]], C = PointGround[CornerPoint[T * 3 + 2]];
					if (A == B || A == C) TriangleGround[T] = A;
					else if (B == C) TriangleGround[T] = B;
				}
				Vote();
			}
		}

		int32 Triangle = 0;
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
				const float G0 = PointGround[CornerPoint[Triangle * 3]], G1 = PointGround[CornerPoint[Triangle * 3 + 1]], G2 = PointGround[CornerPoint[Triangle * 3 + 2]];
				++Triangle;
				TStaticArray<FVertexInstanceID, 3> Corners;
				for (int32 K = 0; K < 3; ++K)
				{
					const FBRRVertex& V = Section.Vertices[T + K];
					const FVertexID Vertex = Desc.CreateVertex();
					Positions[Vertex] = V.Position;
					const FVertexInstanceID Instance = Desc.CreateVertexInstance(Vertex);
					Normals[Instance] = V.Normal.IsNearlyZero() ? Normal : V.Normal;
					UVs.Set(Instance, 0, V.UV);
					UVs.Set(Instance, 1, FVector2f(G0, G1));
					UVs.Set(Instance, 2, FVector2f(G2, 0.f));
					UVs.Set(Instance, 3, FVector2f(K == 0 ? 1.f : 0.f, K == 1 ? 1.f : 0.f)); // interpolated: the corners' weights
					// The build sRGB-encodes the colour back to the palette bytes (M_BRR_Flat); the
					// alpha carries the liquid (ConnectLook).
					FLinearColor Colour(V.Color);
					const EBRRLiquid Liquid = V.Land <= 7 ? Assets.World->LiquidOf(V.Land) : EBRRLiquid::None;
					Colour.A = Liquid == EBRRLiquid::Water ? 2.f / 3.f : Liquid == EBRRLiquid::Ice ? 1.f / 3.f : Liquid == EBRRLiquid::Lava ? 0.f : 1.f;
					if (V.Land == BRR::GlowLand) Colour.A = 0.f; // a lamp: glowing, as the lava
					Colors[Instance] = FVector4f(Colour);
					Corners[K] = Instance;
				}
				Desc.CreateTriangle(Group, Corners);
			}
		}

		if (Source.Sections.Num() && Source.Sections[0].Vertices.Num() && Source.Sections[0].Vertices[0].Land != 0xFF)
		{
			UE_LOG(LogBRRImport, Display, TEXT("%s ground: none %d, dirt %d, grass %d, sand %d, snow %d, rock %d, asphalt %d, regolith %d"), *Name,
				GroundCounts[0], GroundCounts[1], GroundCounts[2], GroundCounts[3], GroundCounts[4], GroundCounts[5], GroundCounts[6], GroundCounts[7]);
		}
		FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
		Model.BuildSettings.bRecomputeNormals = false;
		Model.BuildSettings.bRecomputeTangents = true;
		Model.BuildSettings.bRemoveDegenerates = false;
		Model.BuildSettings.bGenerateLightmapUVs = false;
		Model.BuildSettings.bUseFullPrecisionUVs = true;
		Mesh->CreateMeshDescription(0, MoveTemp(Desc));
		Mesh->CommitMeshDescription(0);
		if (bNanite)
		{
			// The terrain and the decor cover the whole map: as plain meshes they overflow the
			// virtual shadow maps' marking queue (and are drawn into every shadow page whole).
			// Nanite clusters them. The fallback - what ray tracing, Lumen and the collision use -
			// keeps every triangle, so those see the original's geometry exactly.
			Mesh->NaniteSettings.bEnabled = true;
			// Drawn-only meshes (the refined terrain, the scattered detail) take the engine's own fallback.
			Mesh->NaniteSettings.FallbackTarget = bComplexCollision ? ENaniteFallbackTarget::PercentTriangles : ENaniteFallbackTarget::Auto;
			Mesh->NaniteSettings.FallbackPercentTriangles = 1.f;
		}
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

	// Smooth shading for the remastered look (the classic look is unlit: normals change nothing
	// there). Up to these angles between faces: the terrain's rolling ground, the scenery's and
	// the bodies' gentle bends; anything sharper keeps its edge.
	constexpr float TerrainSmoothing = 50.f, DecorSmoothing = 35.f, BodySmoothing = 25.f;
	// The remastered look's round versions (BRR::SubdivideSmooth, twice: 16 triangles for one):
	// folds sharper than these stay creases - a car's panel lines, a building's corners - and the
	// rest (an inflatable's tubes, a tree's crown, a rock) comes out smooth.
	constexpr float BodyCrease = 50.f, DecorCrease = 50.f;
	FBRRMesh Smoothed(const FBRRMesh& Mesh, float MaxAngle)
	{
		FBRRMesh Out = Mesh;
		BRR::SmoothNormals(Out, MaxAngle);
		return Out;
	}

	// The scattered ground detail's shapes (ABRRScatter), shared by every level: four grass tufts
	// and three stones, Nanite, on M_BRR_Scatter.
	void MakeScatterMeshes(const FString& Root, const FMasters& Masters)
	{
		FLevelAssets Shared;
		Shared.Dir = Root;
		Shared.Prefix = TEXT("BRR");
		Shared.Masters = Masters;
		auto Make = [&](const FString& Name, const FBRRMesh& Source)
		{
			UStaticMesh* Mesh = MakeMesh(Shared, Name, Source, false, ESurface::Ground, true);
			if (!Mesh) return;
			for (FStaticMaterial& M : Mesh->GetStaticMaterials()) M.MaterialInterface = Masters.Scatter;
			Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			Mesh->PostEditChange();
			SaveAsset(Mesh);
		};
		for (int32 I = 0; I < 4; ++I) Make(FString::Printf(TEXT("SM_BRR_Grass%d"), I), BRR::MakeGrassTuft(I));
		for (int32 I = 0; I < 3; ++I) Make(FString::Printf(TEXT("SM_BRR_Stone%d"), I), BRR::MakeStone(I, 2));
		// The crowd (ABRRCrowd): each kind of spectator's shirt, tinted per person, and the rest of him.
		for (int32 V = 0; V < BRR::SpectatorVariants; ++V)
		{
			FBRRMesh Shirt, Rest;
			BRR::MakeSpectator(V, Shirt, Rest);
			Make(FString::Printf(TEXT("SM_BRR_Fan%d_Shirt"), V), Shirt);
			Make(FString::Printf(TEXT("SM_BRR_Fan%d_Rest"), V), Rest);
		}
	}

	FVehicleAssets MakeVehicle(FLevelAssets& Assets, const FBRRVehicleMesh& Vehicle)
	{
		FVehicleAssets Out;
		Out.Body = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body"), *Vehicle.Name), Smoothed(Vehicle.Body, BodySmoothing), false, ESurface::Paint);
		FBRRMesh RoundBody = BRR::SubdivideSmooth(Vehicle.Body, 2, BodyCrease);
		BRR::AddVehicleLamps(RoundBody);
		MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body_HD"), *Vehicle.Name), RoundBody, false, ESurface::Paint); // ABRRLook swaps it in
		double Lowest = Vehicle.Body.GetBounds().Min.Z;
		for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
		{
			const FBRRWheel& Wheel = Vehicle.Wheels[I];
			if (UStaticMesh* Mesh = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Wheel%d"), *Vehicle.Name, I), Wheel.Mesh, false, ESurface::Rubber))
			{
				Out.Wheels.Add({ Mesh, Wheel.Socket });
				Lowest = FMath::Min(Lowest, Wheel.Socket.Z + Wheel.Mesh.GetBounds().Min.Z);
				// The remastered look's round one (ABRRLook swaps it in by its _HD name).
				MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Wheel%d_HD"), *Vehicle.Name, I), BRR::RoundWheel(Wheel.Mesh, Wheel.Axle, 4), false, ESurface::Rubber);
			}
		}
		Out.GroundOffset = -Lowest;
		// The cabin view's interior and its parts (the race shows them in place of the body).
		MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Cabin"), *Vehicle.Name), Vehicle.Cabin, false);
		for (int32 I = 0; I < Vehicle.CabinParts.Num(); ++I)
		{
			MakeMesh(Assets, FString::Printf(TEXT("SM_%s_CabinPart%d"), *Vehicle.Name, I), Vehicle.CabinParts[I].Mesh, false);
		}
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

	UStaticMesh* Terrain = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Terrain"), *Level), Smoothed(World.Terrain, TerrainSmoothing), true, ESurface::Ground, true);
	UStaticMesh* Decor = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Decor"), *Level), Smoothed(World.Decor, DecorSmoothing), true, ESurface::Ground, true);
	// The remastered look's scenery, rounded, its plants grown again (BRR::FindTrees, GrowTree):
	// drawn only, the original's collides (as the terrain).
	TBitArray<> PlantTriangles, PropTriangles;
	const TArray<BRR::FBRRTree> Trees = BRR::FindTrees(World, PlantTriangles);
	// ...and its furniture rebuilt (BRR::BuildProps): tyre walls, cones, bales, fences, barriers,
	// posts, rocks, the signs' art in frames.
	int32 PropCount = 0;
	const FBRRMesh Props = BRR::BuildProps(World, PropTriangles, PropCount);
	TBitArray<> Rebuilt = PlantTriangles;
	for (int32 I = 0; I < Rebuilt.Num() && I < PropTriangles.Num(); ++I) Rebuilt[I] = Rebuilt[I] || PropTriangles[I];
	UStaticMesh* DecorHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_DecorHD"), *Level),
		BRR::SubdivideSmooth(BRR::WithoutPlants(World.Decor, Rebuilt), 2, DecorCrease), false, ESurface::Ground, true);
	UE_LOG(LogBRRImport, Display, TEXT("%s: %d pieces of furniture rebuilt (%d triangles)"), *Level, PropCount, Props.NumTriangles());
	UStaticMesh* PropsHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_PropsHD"), *Level), Props, false, ESurface::Ground, true);
	FBRRMesh Grown;
	for (const BRR::FBRRTree& Tree : Trees)
	{
		const FBRRMesh One = BRR::GrowTree(Tree);
		for (const FBRRMeshSection& Section : One.Sections)
		{
			FBRRMeshSection* To = Grown.Sections.FindByPredicate([&](const FBRRMeshSection& S) { return S.bTwoSided == Section.bTwoSided; });
			if (!To) { To = &Grown.Sections.AddDefaulted_GetRef(); To->bTwoSided = Section.bTwoSided; }
			To->Vertices.Append(Section.Vertices);
		}
	}
	UE_LOG(LogBRRImport, Display, TEXT("%s: %d plants grown again (%d triangles)"), *Level, Trees.Num(), Grown.NumTriangles());
	UStaticMesh* TreesHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_TreesHD"), *Level), Grown, false, ESurface::Ground, true);
	// The remastered look's terrain: each triangle cut 4 x 4 onto a smooth surface through the
	// original's heights (FBRRWorld::RefinedTerrain). Drawn only: the original's collides.
	FBRRMesh RefinedLand, Water, WaterBed;
	BRR::SplitWater(World, World.RefinedTerrain(4), RefinedLand, Water, WaterBed);
	UStaticMesh* TerrainHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_TerrainHD"), *Level), RefinedLand, false, ESurface::Refined, true);
	UStaticMesh* WaterHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_WaterHD"), *Level), Water, false);
	UStaticMesh* WaterBedHD = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_WaterBedHD"), *Level), WaterBed, false, ESurface::Ground, true);
	if (WaterHD)
	{
		// The water's colour: the mean of its art, as the original paints it.
		FLinearColor Sum = FLinearColor::Black;
		int32 N = 0;
		for (const FBRRMeshSection& S : Water.Sections)
		{
			for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
			{
				if (World.LiquidOf(S.Vertices[T].Land) != EBRRLiquid::Water) continue; // the banks it reaches over
				++N;
				Sum += FLinearColor::FromSRGBColor(World.Atlases.IsValidIndex(S.Atlas)
					? FBRRWorld::AverageTexels(World.Atlases[S.Atlas], S.Vertices[T].UV, S.Vertices[T + 1].UV, S.Vertices[T + 2].UV) : S.Vertices[T].Color);
			}
		}
		UMaterialInstanceConstant* MI = NewAsset<UMaterialInstanceConstant>(Assets.Dir / TEXT("Materials") / FString::Printf(TEXT("MI_%s_Water"), *Level));
		MI->SetParentEditorOnly(Assets.Masters.Water);
		if (N) MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WaterColour")), Sum / float(N));
		MI->PostEditChange();
		SaveAsset(MI);
		for (FStaticMaterial& M : WaterHD->GetStaticMaterials()) M.MaterialInterface = MI;
		WaterHD->PostEditChange();
		SaveAsset(WaterHD);
	}
	MakeScatterMeshes(Options.DestRoot, Assets.Masters);
	// The ground effects' sheets: no mesh draws them, the race's particles do (ABRRParticles).
	for (int32 Page = 0; Page < World.ParticleAtlasCount; ++Page) Assets.MaterialFor(World.ParticleAtlasBase + Page, true, ESurface::Effect);
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
			MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body_C%d"), *Vehicle.Name, Colour), Smoothed(Vehicle.Body, BodySmoothing), false, ESurface::Paint);
			FBRRMesh RoundRepaint = BRR::SubdivideSmooth(Vehicle.Body, 2, BodyCrease);
			BRR::AddVehicleLamps(RoundRepaint);
			MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body_C%d_HD"), *Vehicle.Name, Colour), RoundRepaint, false, ESurface::Paint);
		}
	}

	// The map.
	OutMapPackage = Assets.Dir / FString::Printf(TEXT("L_%s"), *Level);
	UPackage* MapPackage = MakePackage(OutMapPackage);
	UWorld* Map = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FPackageName::GetLongPackageAssetName(OutMapPackage)), MapPackage);
	Map->SetFlags(AssetFlags);

	if (AStaticMeshActor* Original = SpawnMeshActor(Map, Terrain, FTransform::Identity, TEXT("Terrain")))
	{
		Original->Tags.Add(TEXT("BRRClassicOnly")); // hidden in the remastered look, still solid
	}
	for (const TPair<UStaticMesh*, const TCHAR*>& Drawn : { MakeTuple(TerrainHD, TEXT("TerrainHD")), MakeTuple(WaterHD, TEXT("WaterHD")), MakeTuple(WaterBedHD, TEXT("WaterBedHD")) })
	{
		if (AStaticMeshActor* Refined = SpawnMeshActor(Map, Drawn.Key, FTransform::Identity, Drawn.Value))
		{
			Refined->Tags.Add(TEXT("BRRRemasterOnly"));
			Refined->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		}
	}
	if (AStaticMeshActor* Original = SpawnMeshActor(Map, Decor, FTransform::Identity, TEXT("Decor")))
	{
		if (DecorHD) Original->Tags.Add(TEXT("BRRClassicOnly")); // hidden in the remastered look, still solid
	}
	for (const TPair<UStaticMesh*, const TCHAR*>& Drawn : { MakeTuple(DecorHD, TEXT("DecorHD")), MakeTuple(TreesHD, TEXT("TreesHD")), MakeTuple(PropsHD, TEXT("PropsHD")) })
	{
		if (AStaticMeshActor* Round = SpawnMeshActor(Map, Drawn.Key, FTransform::Identity, Drawn.Value))
		{
			Round->Tags.Add(TEXT("BRRRemasterOnly"));
			Round->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		}
	}

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
