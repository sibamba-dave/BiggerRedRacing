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
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
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

	UTexture2D* MakeTexture(const FString& PackageName, const FBRRImage& Image)
	{
		UTexture2D* Texture = NewAsset<UTexture2D>(PackageName);
		// FColor is BGRA in memory, which is exactly TSF_BGRA8.
		Texture->Source.Init(Image.Width, Image.Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Image.Pixels.GetData()));
		Texture->SRGB = true;
		Texture->Filter = TF_Nearest;
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->CompressionSettings = TC_EditorIcon; // uncompressed: palette art must stay exact
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

	FMasters MakeMasters(const FString& Root)
	{
		FMasters M;
		UMaterialParameterCollection* Collection = NewAsset<UMaterialParameterCollection>(Root / TEXT("Materials/MPC_BRR"));
		{
			FCollectionScalarParameter Distance;
			Distance.ParameterName = TEXT("DrawDistance");
			Distance.DefaultValue = 0.f; // the editor shows the whole map
			Collection->ScalarParameters.Add(Distance);
			Collection->PostEditChange();
			SaveAsset(Collection);
		}
		{
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Textured"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Masked;
			auto* Tex = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
			Tex->ParameterName = TEXT("Atlas");
			Tex->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("RGB"), MP_EmissiveColor);
			auto* Mask = Cast<UMaterialExpressionMultiply>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionMultiply::StaticClass(), -150, 200));
			Mask->A.Connect(4, Tex); // alpha
			Mask->B.Connect(0, MakeDrawRange(Mat, Collection));
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
			Mat->SetShadingModel(MSM_Unlit);
			UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVertexColor::StaticClass(), -500, 0);
			UMaterialExpressionCustom* Decode = MakeCustom(Mat, TEXT(
				"float3 c = saturate(Srgb.rgb);\n"
				"return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));"), CMOT_Float3, { { TEXT("Srgb"), Colour } });
			UMaterialEditingLibrary::ConnectMaterialProperty(Decode, TEXT(""), MP_EmissiveColor);
			Mat->BlendMode = BLEND_Masked;
			UMaterialEditingLibrary::ConnectMaterialProperty(MakeDrawRange(Mat, Collection), TEXT(""), MP_OpacityMask);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Flat = Mat;
		}
		return M;
	}

	// Per-level asset state shared by every mesh: one texture per used atlas page, one material
	// instance per (atlas, sidedness).
	struct FLevelAssets
	{
		const FBRRWorld* World = nullptr;
		FString Dir;      // DestRoot/<LEVEL>
		FString Prefix;   // <LEVEL>
		FMasters Masters;
		TMap<int32, UTexture2D*> Textures;
		TMap<TPair<int32, bool>, UMaterialInstanceConstant*> Materials;

		UMaterialInstanceConstant* MaterialFor(int32 Atlas, bool bTwoSided)
		{
			if (UMaterialInstanceConstant** Found = Materials.Find({ Atlas, bTwoSided })) return *Found;

			const FString Name = FString::Printf(TEXT("MI_%s_%s%s"), *Prefix,
				Atlas == INDEX_NONE ? TEXT("Flat") : *FString::Printf(TEXT("Atlas%02d"), Atlas), bTwoSided ? TEXT("_2S") : TEXT(""));
			UMaterialInstanceConstant* MI = NewAsset<UMaterialInstanceConstant>(Dir / TEXT("Materials") / Name);
			MI->SetParentEditorOnly(Atlas == INDEX_NONE ? Masters.Flat : Masters.Textured);
			if (Atlas != INDEX_NONE)
			{
				UTexture2D*& Texture = Textures.FindOrAdd(Atlas);
				if (!Texture)
				{
					Texture = MakeTexture(Dir / TEXT("Textures") / FString::Printf(TEXT("T_%s_Atlas%02d"), *Prefix, Atlas), World->Atlases[Atlas]);
				}
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Atlas")), Texture);
			}
			if (bTwoSided)
			{
				MI->BasePropertyOverrides.bOverride_TwoSided = true;
				MI->BasePropertyOverrides.TwoSided = true;
			}
			MI->PostEditChange();
			SaveAsset(MI);
			Materials.Add({ Atlas, bTwoSided }, MI);
			return MI;
		}
	};

	UStaticMesh* MakeMesh(FLevelAssets& Assets, const FString& Name, const FBRRMesh& Source, bool bComplexCollision)
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
			UMaterialInstanceConstant* Material = Assets.MaterialFor(Section.Atlas, Section.bTwoSided);
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
					Colors[Instance] = FVector4f(FLinearColor(V.Color)); // the build sRGB-encodes it back to the palette bytes (M_BRR_Flat)
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
		Out.Body = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Body"), *Vehicle.Name), Vehicle.Body, false);
		double Lowest = Vehicle.Body.GetBounds().Min.Z;
		for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
		{
			const FBRRWheel& Wheel = Vehicle.Wheels[I];
			if (UStaticMesh* Mesh = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Wheel%d"), *Vehicle.Name, I), Wheel.Mesh, false))
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

	UStaticMesh* Terrain = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Terrain"), *Level), World.Terrain, true);
	UStaticMesh* Decor = MakeMesh(Assets, FString::Printf(TEXT("SM_%s_Decor"), *Level), World.Decor, true);
	TArray<FVehicleAssets> VehicleAssets;
	for (const FBRRVehicleMesh& Vehicle : World.Vehicles) VehicleAssets.Add(MakeVehicle(Assets, Vehicle));

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
	}
	{
		APostProcessVolume* Look = Map->SpawnActor<APostProcessVolume>();
		Look->SetActorLabel(TEXT("ClassicLook"));
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
