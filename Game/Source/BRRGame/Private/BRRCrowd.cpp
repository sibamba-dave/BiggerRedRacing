#include "BRRCrowd.h"

#include "BRRWorld.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRCrowd, Log, All);

namespace
{
	TAutoConsoleVariable<int32> CVarCrowds(TEXT("brr.Crowds"), 1, TEXT("The remastered look's spectators and waving flags: 1 on, 0 off."));
	// Shirts: the plaids' reds, greens, blues, a mustard and a black-and-red (linear tints).
	const FLinearColor Shirts[] = { {0.55f, 0.06f, 0.05f}, {0.08f, 0.3f, 0.1f}, {0.08f, 0.12f, 0.4f}, {0.5f, 0.38f, 0.08f}, {0.3f, 0.04f, 0.04f}, {0.6f, 0.6f, 0.6f} };
}

ABRRCrowd::ABRRCrowd()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	RootComponent->SetMobility(EComponentMobility::Movable);
	Cloth = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Cloth"));
	Cloth->SetupAttachment(RootComponent);
	Cloth->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cloth->SetCastShadow(false);
	Cloth->SetVisibleInRayTracing(false);
	Cloth->BoundsScale = 2.f; // the shader's wave swings the cloth outside its still-build bounds
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("BRRRemasterOnly"));
}

void ABRRCrowd::Build(const FBRRWorld& World, const TArray<FVector>& Lines)
{
	CrowdMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Crowd.M_BRR_Crowd"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	ClothMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Cloth.M_BRR_Cloth"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!ClothMaterial) ClothMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Scatter.M_BRR_Scatter"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!CrowdMaterial) UE_LOG(LogBRRCrowd, Warning, TEXT("No M_BRR_Crowd: re-import a level; the crowd will stand still"));
	const TArray<BRR::FBRRFlag> Flags = BRR::FindFlags(World);
	if (CVarCrowds.GetValueOnGameThread() == 0) return;

	for (int32 V = 0; V < BRR::SpectatorVariants; ++V)
		for (const TCHAR* Part : { TEXT("Shirt"), TEXT("Rest") })
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/BRR/Meshes/SM_BRR_Fan%d_%s.SM_BRR_Fan%d_%s"), V, Part, V, Part), nullptr, LOAD_NoWarn | LOAD_Quiet);
			UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
			C->SetupAttachment(RootComponent);
			C->SetStaticMesh(Mesh);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->NumCustomDataFloats = 6; // 0-2 tint, 3 phase, 4 pace, 5 jump share
			C->SetCullDistances(0, 25000);
			C->SetCastShadow(false);
			if (CrowdMaterial) C->SetMaterial(0, CrowdMaterial);
			C->RegisterComponent();
			Parts.Add(C);
		}
	if (!Parts[0]->GetStaticMesh())
	{
		UE_LOG(LogBRRCrowd, Warning, TEXT("No spectators: re-import a level for SM_BRR_Fan*"));
		return;
	}

	// Where nobody may stand: the track and the liquids, by cell.
	const int32 Rows = World.MapHeight, Cols = World.MapWidth;
	const double Cell = BRR::TerrainCellCm;
	TBitArray<> Busy(false, Rows * Cols);
	for (const FBRRMeshSection& S : World.Terrain.Sections)
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
		{
			const uint8 Land = S.Vertices[T].Land;
			if (Land != 0 && Land != 1 && World.LiquidOf(Land) == EBRRLiquid::None) continue;
			const FVector3f Mid = (S.Vertices[T].Position + S.Vertices[T + 1].Position + S.Vertices[T + 2].Position) / 3.f;
			const int32 R = FMath::FloorToInt32(Mid.X / Cell), C = FMath::FloorToInt32(Mid.Y / Cell);
			if (R >= 0 && C >= 0 && R < Rows && C < Cols) Busy[R * Cols + C] = true;
		}
	auto Ground = [&](double X, double Y, bool& bOk)
	{
		const int32 R = FMath::FloorToInt32(X / Cell), C = FMath::FloorToInt32(Y / Cell);
		bOk = R >= 0 && C >= 0 && R < Rows - 1 && C < Cols - 1 && !Busy[R * Cols + C] && World.CornerZ.Num() == Rows * Cols;
		if (!bOk) return 0.0;
		const double FR = X / Cell - R, FC = Y / Cell - C;
		auto Z = [&](int32 A, int32 B) { return double(World.CornerZ[A * Cols + B]); };
		return FMath::Lerp(FMath::Lerp(Z(R, C), Z(R, C + 1), FC), FMath::Lerp(Z(R + 1, C), Z(R + 1, C + 1), FC), FR)
			+ World.RefinedOffsetAt(X, Y) - World.ShoreSinkAt(X, Y);
	};

	// The cars' routes on a 10 m grid: a spot near any of them is the track's.
	TSet<FIntPoint> Driven;
	for (const FVector& L : Lines)
		for (int32 DX = -1; DX <= 1; ++DX)
			for (int32 DY = -1; DY <= 1; ++DY) Driven.Add(FIntPoint(FMath::FloorToInt32(L.X / 1000.0) + DX, FMath::FloorToInt32(L.Y / 1000.0) + DY));
	auto NearRoute = [&](const FVector& P)
	{
		if (!Driven.Contains(FIntPoint(FMath::FloorToInt32(P.X / 1000.0), FMath::FloorToInt32(P.Y / 1000.0)))) return false;
		for (const FVector& L : Lines) if (FVector::DistSquared2D(L, P) < 1050.0 * 1050.0) return true;
		return false;
	};
	FRandomStream Rnd(int32(World.MapWidth * 977 + World.Placements.Num()));
	for (const FBRRPlacement& Anchor : World.Placements)
	{
		const FString S = Anchor.Shape.ToUpper();
		const bool bStart = S.StartsWith(TEXT("START")) || S == TEXT("S_GRID");
		if (!bStart && S != TEXT("CHECKP")) continue;
		const FVector Across = Anchor.Rotation.GetRightVector().GetSafeNormal2D(), Along = Anchor.Rotation.GetForwardVector().GetSafeNormal2D();
		for (const float Side : { -1.f, 1.f })
		{
			const int32 Count = bStart ? Rnd.RandRange(30, 45) : Rnd.RandRange(10, 18);
			for (int32 K = 0; K < Count; ++K)
			{
				const FVector Spot = Anchor.Location + Across * (Side * Rnd.FRandRange(1200.f, 2800.f)) + Along * Rnd.FRandRange(-2000.f, 2000.f);
				bool bOk = false;
				const double Z = Ground(Spot.X, Spot.Y, bOk);
				if (!bOk || NearRoute(Spot)) continue;
				const FVector To = (Anchor.Location - Spot).GetSafeNormal2D();
				const FTransform Stand(FRotator(0, To.Rotation().Yaw + Rnd.FRandRange(-30.f, 30.f), 0), FVector(Spot.X, Spot.Y, Z - 2.0), FVector(Rnd.FRandRange(0.92f, 1.1f)));
				const int32 Variant = Rnd.RandRange(0, BRR::SpectatorVariants - 1);
				const float Phase = Rnd.FRandRange(0.f, 2.f * PI), Pace = Rnd.FRandRange(0.8f, 1.4f);
				const float Jump = Variant == 3 ? 0.f : 1.f; // the sitting variant stays seated
				UInstancedStaticMeshComponent* Shirt = Parts[Variant * 2];
				UInstancedStaticMeshComponent* Rest = Parts[Variant * 2 + 1];
				const int32 Instance = Shirt->AddInstance(Stand, true);
				Rest->AddInstance(Stand, true);
				const FLinearColor Tint = Shirts[Rnd.RandRange(0, UE_ARRAY_COUNT(Shirts) - 1)] * 2.f;
				Shirt->SetCustomData(Instance, { Tint.R, Tint.G, Tint.B, Phase, Pace, Jump });
				const float Skin = Rnd.FRandRange(0.8f, 1.15f);
				Rest->SetCustomData(Instance, { Skin, Skin, Skin, Phase, Pace, Jump });
				++PeopleCount;
			}
		}
	}

	// Once, after the build: the instances' custom data reaches the GPU (SetCustomData does not
	// dirty the render state itself; nothing per-frame will).
	for (UInstancedStaticMeshComponent* C : Parts) C->MarkRenderStateDirty();

	// The flags: cloth built once hanging still; the shader runs the wave down it. UV1 carries
	// each flag's width (the wave's reach) and its index (its phase).
	if (!Flags.IsEmpty())
	{
		TArray<FVector> Positions;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs, Seeds, Empty;
		TArray<FColor> Colours;
		constexpr int32 NX = 10, NY = 6;
		for (int32 F = 0; F < Flags.Num(); ++F)
		{
			const BRR::FBRRFlag& Flag = Flags[F];
			const FVector Out = FVector(Flag.Out).GetSafeNormal();
			const FVector Side = FVector::CrossProduct(Out, FVector::UpVector).GetSafeNormal();
			const FColor C(FMath::Min(Flag.Colour.R * 2, 255), FMath::Min(Flag.Colour.G * 2, 255), FMath::Min(Flag.Colour.B * 2, 255));
			const int32 Base = Positions.Num();
			for (int32 Y = 0; Y <= NY; ++Y)
				for (int32 X = 0; X <= NX; ++X)
				{
					const float U = float(X) / NX, V = float(Y) / NY;
					Positions.Add(FVector(Flag.Top) + Out * (U * Flag.Width) - FVector(0, 0, V * Flag.Height + U * U * Flag.Height * 0.08f));
					Normals.Add(Side);
					UVs.Add(FVector2D(U, V));
					Seeds.Add(FVector2D(Flag.Width, F));
					Colours.Add((X / 2 + Y / 2) % 2 ? C : FColor(C.R * 9 / 10, C.G * 9 / 10, C.B * 9 / 10));
				}
			for (int32 Y = 0; Y < NY; ++Y)
				for (int32 X = 0; X < NX; ++X)
				{
					const int32 A = Base + Y * (NX + 1) + X;
					Triangles.Append({ A, A + 1, A + NX + 1, A + 1, A + NX + 2, A + NX + 1 });
				}
		}
		Cloth->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Seeds, Empty, Empty, Colours, TArray<FProcMeshTangent>(), false);
		if (ClothMaterial) Cloth->SetMaterial(0, ClothMaterial);
	}
	UE_LOG(LogBRRCrowd, Display, TEXT("%d spectators, %d flags"), PeopleCount, Flags.Num());
}

void ABRRCrowd::Update(bool bOn)
{
	bOn = bOn && CVarCrowds.GetValueOnGameThread() != 0;
	if (bOn != bShown)
	{
		bShown = bOn;
		SetActorHiddenInGame(!bOn);
	}
}
