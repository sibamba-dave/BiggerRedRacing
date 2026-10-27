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
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("BRRRemasterOnly"));
}

void ABRRCrowd::Build(const FBRRWorld& World, const TArray<FVector>& Lines)
{
	ClothMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Scatter.M_BRR_Scatter"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	Flags = BRR::FindFlags(World);
	if (CVarCrowds.GetValueOnGameThread() == 0) return;

	for (int32 V = 0; V < BRR::SpectatorVariants; ++V)
		for (const TCHAR* Part : { TEXT("Shirt"), TEXT("Rest") })
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/BRR/Meshes/SM_BRR_Fan%d_%s.SM_BRR_Fan%d_%s"), V, Part, V, Part), nullptr, LOAD_NoWarn | LOAD_Quiet);
			UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
			C->SetupAttachment(RootComponent);
			C->SetStaticMesh(Mesh);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->NumCustomDataFloats = 3;
			C->SetCullDistances(0, 25000);
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
				FPerson& P = People.AddDefaulted_GetRef();
				const FVector To = (Anchor.Location - Spot).GetSafeNormal2D();
				P.Stand = FTransform(FRotator(0, To.Rotation().Yaw + Rnd.FRandRange(-30.f, 30.f), 0), FVector(Spot.X, Spot.Y, Z - 2.0), FVector(Rnd.FRandRange(0.92f, 1.1f)));
				P.Variant = Rnd.RandRange(0, BRR::SpectatorVariants - 1);
				P.Phase = Rnd.FRandRange(0.f, 2.f * PI);
				P.Pace = Rnd.FRandRange(0.8f, 1.4f);
				UInstancedStaticMeshComponent* Shirt = Parts[P.Variant * 2];
				UInstancedStaticMeshComponent* Rest = Parts[P.Variant * 2 + 1];
				P.Instance = Shirt->AddInstance(P.Stand, true);
				Rest->AddInstance(P.Stand, true);
				const FLinearColor Tint = Shirts[Rnd.RandRange(0, UE_ARRAY_COUNT(Shirts) - 1)] * 2.f;
				Shirt->SetCustomData(P.Instance, { Tint.R, Tint.G, Tint.B });
				const float Skin = Rnd.FRandRange(0.8f, 1.15f);
				Rest->SetCustomData(P.Instance, { Skin, Skin, Skin });
			}
		}
	}
	UE_LOG(LogBRRCrowd, Display, TEXT("%d spectators, %d flags"), People.Num(), Flags.Num());
}

void ABRRCrowd::Update(const TArray<FTransform>& Racers, double Seconds, bool bOn)
{
	if (bOn != bShown)
	{
		bShown = bOn;
		SetActorHiddenInGame(!bOn);
	}
	if (!bOn) return;
	// The crowd: everybody bobbing a little, jumping up as a car comes near.
	for (FPerson& P : People)
	{
		float Near = 1e9f;
		for (const FTransform& R : Racers) Near = FMath::Min(Near, float(FVector::Dist2D(R.GetLocation(), P.Stand.GetLocation())));
		const float Want = Near < 4000.f ? 1.f : 0.f;
		P.Excitement = FMath::Lerp(P.Excitement, Want, Want > P.Excitement ? 0.15f : 0.02f);
		const float T = float(Seconds) * P.Pace * 6.f + P.Phase;
		const bool bSitting = P.Variant == 3;
		const float Jump = bSitting ? 0.f : FMath::Max(0.f, FMath::Sin(T)) * 28.f * P.Excitement;
		const float Sway = FMath::Sin(T * 0.37f) * (2.f + 6.f * P.Excitement);
		FTransform Now = P.Stand;
		Now.AddToTranslation(FVector(0, 0, Jump));
		Now.SetRotation(P.Stand.GetRotation() * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(Sway)));
		Parts[P.Variant * 2]->UpdateInstanceTransform(P.Instance, Now, true, false, true);
		Parts[P.Variant * 2 + 1]->UpdateInstanceTransform(P.Instance, Now, true, false, true);
	}
	for (UInstancedStaticMeshComponent* C : Parts) C->MarkRenderStateDirty();

	// The flags: cloth flying off the pole, a wave running down it.
	if (Flags.IsEmpty() || CVarCrowds.GetValueOnGameThread() == 0) return;
	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
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
				const float Wave = FMath::Sin(U * 7.f - float(Seconds) * 6.f + F) * Flag.Width * 0.07f * U + FMath::Sin(U * 13.f - float(Seconds) * 9.f) * Flag.Width * 0.02f * U;
				Positions.Add(FVector(Flag.Top) + Out * (U * Flag.Width) - FVector(0, 0, V * Flag.Height + U * U * Flag.Height * 0.08f) + Side * Wave);
				Normals.Add(Side);
				UVs.Add(FVector2D(U, V));
				Colours.Add((X / 2 + Y / 2) % 2 ? C : FColor(C.R * 9 / 10, C.G * 9 / 10, C.B * 9 / 10));
			}
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
			{
				const int32 A = Base + Y * (NX + 1) + X;
				Triangles.Append({ A, A + 1, A + NX + 1, A + 1, A + NX + 2, A + NX + 1 });
			}
	}
	Cloth->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
	if (ClothMaterial) Cloth->SetMaterial(0, ClothMaterial);
}
