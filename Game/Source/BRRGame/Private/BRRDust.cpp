#include "BRRDust.h"

#include "BRRWorld.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRDust, Log, All);

namespace
{
	constexpr int32 MaxPuffs = 360;
	constexpr double PuffSeconds = 0.12;  // a puff per racer every so often at speed
	constexpr double MinSpeed = 700.0;    // cm/s before a car raises any
}

ABRRDust::ABRRDust()
{
	Haze = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Haze"));
	Haze->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Haze->SetCastShadow(false);
	SetRootComponent(Haze);
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("BRRRemasterOnly"));
}

void ABRRDust::Build(const FBRRWorld& World)
{
	Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Haze.M_BRR_Haze"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	Rows = World.MapHeight;
	Cols = World.MapWidth;
	CornerZ = World.CornerZ;
	const double Cell = BRR::TerrainCellCm;
	// Each cell's dusty ground: the mean colour of its dirt, sand and moon dust, if most of it is.
	TArray<FLinearColor> Sum;
	TArray<float> Dusty, All;
	Sum.Init(FLinearColor::Transparent, Rows * Cols);
	Dusty.Init(0.f, Rows * Cols);
	All.Init(0.f, Rows * Cols);
	for (const FBRRMeshSection& S : World.Terrain.Sections)
	{
		const FBRRImage* Page = World.Atlases.IsValidIndex(S.Atlas) ? &World.Atlases[S.Atlas] : nullptr;
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
		{
			const FVector3f Mid = (S.Vertices[T].Position + S.Vertices[T + 1].Position + S.Vertices[T + 2].Position) / 3.f;
			const int32 R = FMath::FloorToInt32(Mid.X / Cell), C = FMath::FloorToInt32(Mid.Y / Cell);
			if (R < 0 || C < 0 || R >= Rows || C >= Cols) continue;
			All[R * Cols + C] += 1.f;
			if (World.LiquidOf(S.Vertices[T].Land) != EBRRLiquid::None) continue;
			const FColor Average = Page ? FBRRWorld::AverageTexels(*Page, S.Vertices[T].UV, S.Vertices[T + 1].UV, S.Vertices[T + 2].UV) : S.Vertices[T].Color;
			const EBRRGround Ground = World.GroundOf(S.Vertices[T].Land, Average);
			if (Ground != EBRRGround::Dirt && Ground != EBRRGround::Sand && Ground != EBRRGround::Regolith) continue;
			Sum[R * Cols + C] += FLinearColor::FromSRGBColor(Average);
			Dusty[R * Cols + C] += 1.f;
		}
	}
	CellDust.Init(FLinearColor::Transparent, Rows * Cols);
	int32 Cells = 0;
	for (int32 I = 0; I < Rows * Cols; ++I)
	{
		if (Dusty[I] < 1.f || Dusty[I] < All[I] * 0.5f) continue;
		// A cloud of it is paler than the ground: fine dust, lit through.
		FLinearColor Colour = Sum[I] / Dusty[I];
		Colour = FMath::Lerp(Colour, FLinearColor(0.82f, 0.78f, 0.72f), 0.65f);
		Colour.A = 1.f;
		CellDust[I] = Colour;
		++Cells;
	}
	UE_LOG(LogBRRDust, Display, TEXT("%s: %d dusty cells%s"), *World.LevelName, Cells, Material ? TEXT("") : TEXT(", M_BRR_Haze missing: re-import a level"));
}

void ABRRDust::Update(const TArray<FTransform>& Poses, const FVector& CameraRight, const FVector& CameraUp, float DeltaSeconds, bool bOn)
{
	if (!bOn || !Material || Rows == 0)
	{
		if (Haze->GetNumSections() > 0) Haze->ClearAllMeshSections();
		Puffs.Reset();
		Racers.Reset();
		return;
	}
	const double Dt = FMath::Max(double(DeltaSeconds), 1e-4);
	const double Cell = BRR::TerrainCellCm;
	Racers.SetNum(Poses.Num());
	for (int32 I = 0; I < Poses.Num(); ++I)
	{
		FRacer& Racer = Racers[I];
		const FVector At = Poses[I].GetLocation();
		const FVector Velocity = Racer.bHaveLast ? (At - Racer.Last) / Dt : FVector::ZeroVector;
		const bool bJump = Racer.bHaveLast && FVector::Dist(At, Racer.Last) > 1500.0;
		Racer.Last = At;
		Racer.bHaveLast = true;
		Racer.Since += Dt;
		const double Speed = Velocity.Size2D();
		if (bJump || Speed < MinSpeed || Racer.Since < PuffSeconds) continue;
		const int32 R = FMath::FloorToInt32(At.X / Cell), C = FMath::FloorToInt32(At.Y / Cell);
		if (R < 0 || C < 0 || R >= Rows - 1 || C >= Cols - 1 || CellDust[R * Cols + C].A <= 0.f) continue;
		// Only off the ground: the corner heights between the cell's corners.
		const double FR = At.X / Cell - R, FC = At.Y / Cell - C;
		auto Z = [&](int32 CR, int32 CC) { return double(CornerZ[CR * Cols + CC]); };
		const double Ground = FMath::Lerp(FMath::Lerp(Z(R, C), Z(R, C + 1), FC), FMath::Lerp(Z(R + 1, C), Z(R + 1, C + 1), FC), FR);
		if (At.Z - Ground > 250.0) continue;
		Racer.Since = 0;
		if (Puffs.Num() >= MaxPuffs) Puffs.RemoveAt(0);
		const double Fast = FMath::Clamp((Speed - MinSpeed) / 3000.0, 0.0, 1.0);
		const FVector Back = -Velocity.GetSafeNormal2D();
		FPuff& P = Puffs.AddDefaulted_GetRef();
		P.Position = FVector(At.X, At.Y, Ground + 120.0) + Back * 180.0 + FVector(Back.Y, -Back.X, 0) * Random.FRandRange(-120.0, 120.0);
		P.Velocity = Velocity * 0.15 + FVector(Random.FRandRange(-40.0, 40.0), Random.FRandRange(-40.0, 40.0), Random.FRandRange(15.0, 40.0));
		P.Colour = CellDust[R * Cols + C];
		P.Life = Random.FRandRange(4.0, 6.5);
		P.Size0 = 150.0 + 100.0 * Fast;
		P.Size1 = 700.0 + 600.0 * Fast;
		P.Strength = 0.3 + 0.3 * Fast;
		P.Spin = Random.FRandRange(0.f, 1.f);
	}
	for (FPuff& P : Puffs)
	{
		P.Age += Dt;
		P.Position += P.Velocity * Dt;
		P.Velocity *= FMath::Exp(-1.2 * Dt); // the air takes its speed
		P.Velocity.Z += 6.0 * Dt;
	}
	Puffs.RemoveAll([](const FPuff& P) { return P.Age > P.Life; });

	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	TArray<FColor> Colours;
	// Far ones first: translucent cards over each other.
	const FVector Facing = CameraRight ^ CameraUp;
	TArray<int32> Order;
	for (int32 I = 0; I < Puffs.Num(); ++I) Order.Add(I);
	Order.Sort([&](int32 A, int32 B) { return (Puffs[A].Position | Facing) > (Puffs[B].Position | Facing); });
	for (const int32 I : Order)
	{
		const FPuff& P = Puffs[I];
		const double T = P.Age / P.Life;
		const double Size = FMath::Lerp(P.Size0, P.Size1, 1.0 - FMath::Square(1.0 - T)) * 0.5;
		const double Alpha = P.Strength * FMath::Min(P.Age / 0.4, 1.0) * FMath::Square(1.0 - T);
		const FVector R = CameraRight * Size, U = CameraUp * Size;
		const int32 Base = Positions.Num();
		Positions.Append({ P.Position - R - U, P.Position + R - U, P.Position - R + U, P.Position + R + U });
		// UV across the card, a whole number added to x to seed its noise, so no two look alike.
		const double Seed = FMath::FloorToDouble(P.Spin * 50.0);
		UVs.Append({ FVector2D(Seed, 0), FVector2D(Seed + 1, 0), FVector2D(Seed, 1), FVector2D(Seed + 1, 1) });
		static const IConsoleVariable* Test = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.WakeDebug"));
		const bool bTest = Test && Test->GetInt() == 2;
		const FColor C = bTest ? FColor::White : FLinearColor(P.Colour.R, P.Colour.G, P.Colour.B, float(FMath::Clamp(Alpha, 0.0, 1.0))).ToFColor(false);
		Colours.Append({ C, C, C, C });
		Triangles.Append({ Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
	}
	if (Positions.IsEmpty())
	{
		if (Haze->GetNumSections() > 0) Haze->ClearAllMeshSections();
		return;
	}
	static double Clock = 0, Logged = 0;
	Clock += Dt;
	if (Clock - Logged > 5.0)
	{
		Logged = Clock;
		static const IConsoleVariable* Perf = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.LogPerf"));
		if (Perf && Perf->GetBool()) UE_LOG(LogBRRDust, Display, TEXT("dust: %d puffs"), Puffs.Num());
	}
	TArray<FVector> Normals;
	Normals.Init(-Facing, Positions.Num());
	Haze->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
	static const IConsoleVariable* Debug = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.WakeDebug"));
	static UMaterialInterface* Plain = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Haze->SetMaterial(0, Debug && Debug->GetInt() == 1 && Plain ? Plain : Material.Get());
}
