#include "BRRWake.h"

#include "BRRWorld.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRWake, Log, All);
static TAutoConsoleVariable<int32> CVarWakeDebug(TEXT("brr.WakeDebug"), 0, TEXT("1: draws the wakes and the dust plain and opaque; 2: the dust white and solid in its own material."));

namespace
{
	constexpr double CrumbSeconds = 0.05;   // a wake point every so often
	constexpr double KelvinSpread = 0.35;   // tan(19.5 deg): how fast an arm moves out per unit behind
	constexpr double HalfBeam = 90.0;       // cm, a racing boat's half width
	constexpr double ChurnSeconds = 1.6;
	constexpr double SplashSeconds = 0.7;
	constexpr double MaxReach = 2500.0;     // cm an arm spreads before it is lost in the chop
}

ABRRWake::ABRRWake()
{
	Foam = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Foam"));
	Foam->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Foam->SetCastShadow(false);
	SetRootComponent(Foam);
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("BRRRemasterOnly"));
}

void ABRRWake::Build(const FBRRWorld& World)
{
	Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Foam.M_BRR_Foam"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Material) UE_LOG(LogBRRWake, Warning, TEXT("M_BRR_Foam missing: re-import a level for the wakes"));
	Rows = World.MapHeight;
	Cols = World.MapWidth;
	const double Cell = BRR::TerrainCellCm;
	CellLevel.Init(NAN, Rows * Cols);
	for (const FBRRMeshSection& S : World.Terrain.Sections)
	{
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
		{
			if (World.LiquidOf(S.Vertices[T].Land) != EBRRLiquid::Water) continue;
			const FVector3f Mid = (S.Vertices[T].Position + S.Vertices[T + 1].Position + S.Vertices[T + 2].Position) / 3.f;
			const int32 R = FMath::FloorToInt32(Mid.X / Cell), C = FMath::FloorToInt32(Mid.Y / Cell);
			if (R < 0 || C < 0 || R >= Rows || C >= Cols) continue;
			float& L = CellLevel[R * Cols + C];
			L = FMath::IsNaN(L) ? Mid.Z : FMath::Min(L, Mid.Z);
		}
	}
	// The water reaches a cell into the let-down banks (BRR::SplitWater): so do the wakes.
	{
		// The drawn water's height (the original's water triangles, BRR::SplitWater), sampled at
		// the Sub grid's points: a river runs downhill.
		const double Step = Cell / Sub;
		const int32 SC = Cols * Sub;
		Surface.Init(NAN, Rows * Sub * SC);
		for (const FBRRMeshSection& S : World.Terrain.Sections)
		{
			for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
			{
				if (World.LiquidOf(S.Vertices[T].Land) != EBRRLiquid::Water) continue;
				const FVector3f A = S.Vertices[T].Position, B = S.Vertices[T + 1].Position, C = S.Vertices[T + 2].Position;
				const int32 R0 = FMath::Max(FMath::FloorToInt32(FMath::Min3(A.X, B.X, C.X) / Step), 0), R1 = FMath::Min(FMath::CeilToInt32(FMath::Max3(A.X, B.X, C.X) / Step), Rows * Sub - 1);
				const int32 C0 = FMath::Max(FMath::FloorToInt32(FMath::Min3(A.Y, B.Y, C.Y) / Step), 0), C1 = FMath::Min(FMath::CeilToInt32(FMath::Max3(A.Y, B.Y, C.Y) / Step), SC - 1);
				const double Den = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
				if (FMath::Abs(Den) < 1e-3) continue;
				for (int32 R = R0; R <= R1; ++R)
					for (int32 Q = C0; Q <= C1; ++Q)
					{
						const double X = (R + 0.5) * Step, Y = (Q + 0.5) * Step;
						const double WA = ((B.Y - C.Y) * (X - C.X) + (C.X - B.X) * (Y - C.Y)) / Den;
						const double WB = ((C.Y - A.Y) * (X - C.X) + (A.X - C.X) * (Y - C.Y)) / Den;
						const double WC = 1.0 - WA - WB;
						if (WA < -0.01 || WB < -0.01 || WC < -0.01) continue;
						float& Z = Surface[R * SC + Q];
						const float Here = float(WA * A.Z + WB * B.Z + WC * C.Z);
						Z = FMath::IsNaN(Z) ? Here : FMath::Max(Z, Here);
					}
			}
		}
	}
	TArray<float> Wet = CellLevel;
	for (int32 R = 0; R < Rows; ++R)
		for (int32 C = 0; C < Cols; ++C)
		{
			if (!FMath::IsNaN(Wet[R * Cols + C])) continue;
			for (int32 DR = -1; DR <= 1; ++DR)
				for (int32 DC = -1; DC <= 1; ++DC)
				{
					const int32 NR = R + DR, NC = C + DC;
					if (NR < 0 || NC < 0 || NR >= Rows || NC >= Cols || FMath::IsNaN(Wet[NR * Cols + NC])) continue;
					float& L = CellLevel[R * Cols + C];
					L = FMath::IsNaN(L) ? Wet[NR * Cols + NC] : FMath::Min(L, Wet[NR * Cols + NC]);
				}
		}

	// The remastered ground near the water: the physics' corner heights between the corners, plus
	// the refinement, less the banks' let-down (FBRRWorld::RefinedTerrain's surface).
	const int32 GR = Rows * Sub, GC = Cols * Sub;
	Ground.Init(TNumericLimits<float>::Max(), GR * GC);
	if (World.CornerZ.Num() != Rows * Cols) return;
	auto Corner = [&](int32 R, int32 C) { return double(World.CornerZ[FMath::Clamp(R, 0, Rows - 1) * Cols + FMath::Clamp(C, 0, Cols - 1)]); };
	for (int32 R = 0; R < Rows; ++R)
		for (int32 C = 0; C < Cols; ++C)
		{
			if (FMath::IsNaN(CellLevel[R * Cols + C])) continue;
			for (int32 SR = 0; SR < Sub; ++SR)
				for (int32 SC = 0; SC < Sub; ++SC)
				{
					const double FR = (SR + 0.5) / Sub, FC = (SC + 0.5) / Sub;
					const double X = (R + FR) * Cell, Y = (C + FC) * Cell;
					const double Flat = FMath::Lerp(FMath::Lerp(Corner(R, C), Corner(R, C + 1), FC), FMath::Lerp(Corner(R + 1, C), Corner(R + 1, C + 1), FC), FR);
					Ground[(R * Sub + SR) * GC + C * Sub + SC] = float(Flat + World.RefinedOffsetAt(X, Y) - World.ShoreSinkAt(X, Y));
				}
		}
}

bool ABRRWake::LevelAt(const FVector& P, float& OutLevel) const
{
	const int32 R = FMath::FloorToInt32(P.X / BRR::TerrainCellCm), C = FMath::FloorToInt32(P.Y / BRR::TerrainCellCm);
	if (R < 0 || C < 0 || R >= Rows || C >= Cols) return false;
	OutLevel = CellLevel[R * Cols + C];
	if (FMath::IsNaN(OutLevel)) return false;
	const double Step = BRR::TerrainCellCm / Sub;
	const int32 SR = FMath::FloorToInt32(P.X / Step), SC = FMath::FloorToInt32(P.Y / Step);
	if (SR >= 0 && SC >= 0 && SR < Rows * Sub && SC < Cols * Sub && !FMath::IsNaN(Surface[SR * Cols * Sub + SC])) OutLevel = Surface[SR * Cols * Sub + SC];
	return true;
}

float ABRRWake::GroundAt(const FVector& P) const
{
	const double Step = BRR::TerrainCellCm / Sub;
	const int32 R = FMath::FloorToInt32(P.X / Step), C = FMath::FloorToInt32(P.Y / Step);
	if (R < 0 || C < 0 || R >= Rows * Sub || C >= Cols * Sub) return TNumericLimits<float>::Max();
	return Ground[R * Cols * Sub + C];
}

void ABRRWake::Update(const TArray<FTransform>& Poses, float DeltaSeconds, bool bOn)
{
	if (!bOn || !Material || Rows == 0)
	{
		if (Foam->GetNumSections() > 0) Foam->ClearAllMeshSections();
		Trails.Reset();
		Splashes.Reset();
		return;
	}
	const double Dt = FMath::Max(double(DeltaSeconds), 1e-4);
	Clock += Dt;
	Trails.SetNum(Poses.Num());

	// The trails: a point while on the water and moving; every point ages, and its arms run out
	// until they meet the bank, where they splash.
	for (int32 I = 0; I < Poses.Num(); ++I)
	{
		FTrail& Trail = Trails[I];
		const FVector At = Poses[I].GetLocation();
		const double Moved = Trail.bHaveLast ? FVector::Dist2D(At, Trail.Last) : 0.0;
		// Which way it is going (the way it points need not be: a boat slides).
		if (Moved > 1.0) Trail.Heading = FMath::Lerp(Trail.Heading, (At - Trail.Last).GetSafeNormal2D(), FMath::Min(Dt * 10.0, 1.0)).GetSafeNormal2D();
		Trail.Speed = FMath::Lerp(Trail.Speed, FMath::Min(Moved / Dt, 6000.0), FMath::Min(Dt * 6.0, 1.0));
		const bool bJump = Moved > 1500.0; // a recovery or a reset, not a run
		Trail.Last = At;
		Trail.bHaveLast = true;
		float Level = 0;
		Trail.bOnWater = LevelAt(At, Level) && FMath::Abs(At.Z - Level) < 250.0 && GroundAt(At) < Level;
		for (FCrumb& Crumb : Trail.Crumbs) Crumb.Age += Dt;
		Trail.Crumbs.RemoveAll([](const FCrumb& C) { return C.Age > Lifetime; });
		Trail.SinceCrumb += Dt;
		if (bJump) Trail.Crumbs.Reset();
		if (Trail.bOnWater && Trail.Speed > 250.0 && Trail.SinceCrumb >= CrumbSeconds)
		{
			Trail.SinceCrumb = 0;
			FCrumb& Crumb = Trail.Crumbs.AddDefaulted_GetRef();
			Crumb.Position = FVector(At.X, At.Y, Level + 5.0);
			Crumb.Forward = Trail.Heading.IsNearlyZero() ? Poses[I].GetUnitAxis(EAxis::X).GetSafeNormal2D() : Trail.Heading;
			Crumb.Right = FVector(-Crumb.Forward.Y, Crumb.Forward.X, 0);
			Crumb.Speed = Trail.Speed;
		}
		for (FCrumb& Crumb : Trail.Crumbs)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (Crumb.Landed[Side] > 0) continue;
				const double Reach = HalfBeam + Crumb.Age * Crumb.Speed * KelvinSpread;
				if (Reach > MaxReach) continue;
				const FVector Arm = Crumb.Position + Crumb.Right * (Side ? Reach : -Reach);
				float ArmLevel = float(Crumb.Position.Z - 5.0);
				LevelAt(Arm, ArmLevel);
				if (GroundAt(Arm) > ArmLevel + 2.0)
				{
					Crumb.Landed[Side] = Crumb.Age;
					if (Splashes.Num() < 200)
					{
						FSplash& S = Splashes.AddDefaulted_GetRef();
						S.Position = FVector(Arm.X, Arm.Y, ArmLevel);
						S.Across = Crumb.Forward;
						S.Size = FMath::Clamp(Crumb.Speed / 2500.0, 0.3, 1.5) * FMath::Clamp(1.0 - Crumb.Age / Lifetime, 0.2, 1.0);
					}
				}
			}
		}
	}
	for (FSplash& S : Splashes) S.Age += Dt;
	Splashes.RemoveAll([](const FSplash& S) { return S.Age > SplashSeconds; });

	// The mesh: strips of quads, UV.y across a strip (its edges soft), the vertex alpha its strength.
	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	TArray<FColor> Colours;
	auto Quad = [&](const FVector& A0, const FVector& A1, const FVector& B0, const FVector& B1, double AlphaA, double AlphaB, double U0, double U1)
	{
		const int32 Base = Positions.Num();
		Positions.Append({ A0, A1, B0, B1 });
		UVs.Append({ FVector2D(U0, 0), FVector2D(U0, 1), FVector2D(U1, 0), FVector2D(U1, 1) });
		const uint8 QA = uint8(FMath::Clamp(AlphaA, 0.0, 1.0) * 255), QB = uint8(FMath::Clamp(AlphaB, 0.0, 1.0) * 255);
		Colours.Append({ FColor(255, 255, 255, QA), FColor(255, 255, 255, QA), FColor(255, 255, 255, QB), FColor(255, 255, 255, QB) });
		Triangles.Append({ Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
	};
	for (const FTrail& Trail : Trails)
	{
		const TArray<FCrumb>& C = Trail.Crumbs;
		for (int32 K = 0; K + 1 < C.Num(); ++K)
		{
			const FCrumb& A = C[K];
			const FCrumb& B = C[K + 1];
			if (A.Age - B.Age > CrumbSeconds * 4) continue; // a gap: off the water for a while
			const double FastA = FMath::Clamp(A.Speed / 2000.0, 0.0, 1.0), FastB = FMath::Clamp(B.Speed / 2000.0, 0.0, 1.0);
			// The churn behind the hull.
			if (A.Age < ChurnSeconds)
			{
				auto Width = [](const FCrumb& X) { return HalfBeam * (1.0 + X.Age * 1.2); };
				auto Strength = [&](const FCrumb& X, double Fast) { return (1.0 - X.Age / ChurnSeconds) * (0.35 + 0.5 * Fast); };
				Quad(A.Position - A.Right * Width(A), A.Position + A.Right * Width(A), B.Position - B.Right * Width(B), B.Position + B.Right * Width(B),
					Strength(A, FastA), Strength(B, FastB), A.Age * 0.7, B.Age * 0.7);
			}
			// The arms.
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const double Sign = Side ? 1.0 : -1.0;
				auto Ended = [&](const FCrumb& X) { return X.Landed[Side] > 0; };
				if (Ended(A) || Ended(B)) continue;
				auto Centre = [&](const FCrumb& X) { return X.Position + X.Right * (Sign * (HalfBeam + X.Age * X.Speed * KelvinSpread)); };
				auto Half = [](const FCrumb& X) { return 40.0 + X.Age * 70.0; };
				auto Strength = [&](const FCrumb& X, double Fast) { return FMath::Pow(1.0 - X.Age / Lifetime, 1.5) * Fast * 0.5; };
				const FVector CA = Centre(A), CB = Centre(B);
				if (FVector::Dist2D(CA, A.Position) > MaxReach) continue;
				Quad(CA - A.Right * Half(A), CA + A.Right * Half(A), CB - B.Right * Half(B), CB + B.Right * Half(B),
					Strength(A, FastA), Strength(B, FastB), A.Age * 0.5 + Side * 0.37, B.Age * 0.5 + Side * 0.37);
			}
		}
		// The bow spray: two sheets thrown out and up from the sides at speed.
		if (Trail.bOnWater && Trail.Speed > 1200.0 && C.Num())
		{
			const FCrumb& Now = C.Last();
			const double Fast = FMath::Clamp((Trail.Speed - 1200.0) / 2500.0, 0.0, 1.0);
			const double Flicker = 0.75 + 0.25 * FMath::Sin(Clock * 37.0 + Now.Position.X * 0.01);
			for (const double Sign : { -1.0, 1.0 })
			{
				const FVector Root = Now.Position + Now.Forward * 60.0 + Now.Right * (Sign * HalfBeam * 0.9);
				const FVector Out = Now.Right * (Sign * (40.0 + 120.0 * Fast)) - Now.Forward * (60.0 + 140.0 * Fast) + FVector(0, 0, 50.0 + 90.0 * Fast * Flicker);
				Quad(Root - Now.Forward * 60.0, Root + Out - Now.Forward * 120.0, Root + Now.Forward * 20.0, Root + Out, 0.7 * Fast, 0.0, 0.1, 0.5);
			}
		}
	}
	// The splashes against the bank: two crossed sheets shooting up and falling back.
	for (const FSplash& S : Splashes)
	{
		const double T = S.Age / SplashSeconds;
		const double Height = S.Size * 220.0 * FMath::Sin(T * PI * 0.8);
		const double Width = S.Size * (70.0 + 110.0 * T);
		const double Alpha = (1.0 - T) * 0.7;
		const FVector Side = FVector(-S.Across.Y, S.Across.X, 0);
		for (const FVector& Dir : { S.Across, Side })
		{
			Quad(S.Position - Dir * Width, S.Position - Dir * Width * 0.7 + FVector(0, 0, Height), S.Position + Dir * Width, S.Position + Dir * Width * 0.7 + FVector(0, 0, Height),
				Alpha, Alpha, S.Age * 2.0, S.Age * 2.0 + 0.6);
		}
	}

	if (Positions.IsEmpty())
	{
		if (Foam->GetNumSections() > 0) Foam->ClearAllMeshSections();
		return;
	}
	static double Logged = 0;
	if (Clock - Logged > 5.0)
	{
		Logged = Clock;
		static const IConsoleVariable* Perf = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.LogPerf"));
		if (Perf && Perf->GetBool())
		{
			int32 Crumbs = 0, OnWater = 0;
			for (const FTrail& T : Trails) Crumbs += T.Crumbs.Num(), OnWater += T.bOnWater;
			UE_LOG(LogBRRWake, Display, TEXT("wake: %d on the water, %d points, %d splashes, %d quads"), OnWater, Crumbs, Splashes.Num(), Positions.Num() / 4);
		}
	}
	TArray<FVector> Normals;
	Normals.Init(FVector::UpVector, Positions.Num());
	Foam->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
	static const IConsoleVariable* Debug = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.WakeDebug"));
	static UMaterialInterface* Plain = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Foam->SetMaterial(0, Debug && Debug->GetInt() == 1 && Plain ? Plain : Material.Get());
}
