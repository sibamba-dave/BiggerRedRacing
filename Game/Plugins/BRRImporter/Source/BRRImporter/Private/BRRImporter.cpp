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
	};

	FMasters MakeMasters(const FString& Root)
	{
		FMasters M;
		{
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Textured"));
			Mat->SetShadingModel(MSM_Unlit);
			Mat->BlendMode = BLEND_Masked;
			auto* Tex = Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
			Tex->ParameterName = TEXT("Atlas");
			Tex->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("RGB"), MP_EmissiveColor);
			UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("A"), MP_OpacityMask);
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			SaveAsset(Mat);
			M.Textured = Mat;
		}
		{
			UMaterial* Mat = NewAsset<UMaterial>(Root / TEXT("Materials/M_BRR_Flat"));
			Mat->SetShadingModel(MSM_Unlit);
			UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionVertexColor::StaticClass(), -300, 0);
			UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_EmissiveColor);
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
					Colors[Instance] = FVector4f(FLinearColor(V.Color));
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
