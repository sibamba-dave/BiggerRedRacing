#include "BRRArchive.h"
#include "BRRRace.h"
#include "BRRWorld.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRHandlingTest, "BRR.Formats.Race.JeepHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRHandlingTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FBRRHandlingMetrics M;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("measure JEEP %s"), *Error), BRR::MeasureHandling(DataDir, TEXT("JEEP"), M, Error))) return false;
	for (int32 S = 0; S < 8; ++S)
	{
		const FBRRSurfaceMetrics& X = M.Surfaces[S];
		AddInfo(FString::Printf(TEXT("JEEP %-6s top %6.0f cm/s (%5.1f km/h) t60 %4.2fs t90 %4.2fs brake %4.2fs rev %5.0f turn %5.1f deg/s @ %5.0f cm/s"),
			FBRRHandlingMetrics::SurfaceName(S), X.TopSpeed, X.TopSpeed * 0.036, X.TimeTo60, X.TimeTo90, X.BrakeTime,
			X.ReverseTopSpeed, X.TurnYawRate, X.TurnSpeed));
	}
	for (const FVector2D& P : M.Surfaces[0].TurnCurve) AddInfo(FString::Printf(TEXT("JEEP TRACK power lock at %5.0f cm/s -> %5.1f deg/s"), P.X, P.Y));
	for (const FVector2D& P : M.Surfaces[0].CoastTurnCurve) AddInfo(FString::Printf(TEXT("JEEP TRACK coast lock at %5.0f cm/s -> %5.1f deg/s"), P.X, P.Y));
	AddInfo(FString::Printf(TEXT("JEEP TRACK yaw time constant %.2f s"), M.Surfaces[0].YawTimeConstant));
	TestTrue(TEXT("track top speed plausible"), M.Surfaces[0].TopSpeed > 500 && M.Surfaces[0].TopSpeed < 10000);
	TestTrue(TEXT("accelerates"), M.Surfaces[0].TimeTo90 > 0);
	return true;
}

// A DIRT1 race with the player parked on the grid: the ghosts must leave, lap and be placed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRRaceDirt1Test, "BRR.Formats.Race.Dirt1Ghosts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRRaceDirt1Test::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FString Error;
	TUniquePtr<FBRRRace> Race = FBRRRace::Create(DataDir, TEXT("DIRT1"), TEXT("JEEP"), Error);
	if (!TestTrue(FString::Printf(TEXT("create %s"), *Error), Race.IsValid())) return false;

	const int32 Player = Race->PlayerIndex();
	const FTransform Parked = Race->RacerTransform(Player);
	for (int32 R = 0; R < Race->NumRacers(); ++R)
	{
		AddInfo(FString::Printf(TEXT("racer %d %s colour %d %s at %s yaw %.1f"), R, *Race->VehicleOf(R), Race->ColourOf(R),
			R == Player ? TEXT("PLAYER") : Race->IsGhost(R) ? TEXT("ghost") : TEXT("empty"),
			*Race->RacerTransform(R).GetLocation().ToString(), Race->RacerTransform(R).Rotator().Yaw));
	}

	TArray<FVector> Start;
	for (int32 R = 0; R < Race->NumRacers(); ++R) Start.Add(Race->RacerTransform(R).GetLocation());
	int32 TicksToGo = 0;
	while (!Race->HasStarted() && TicksToGo < 1000)
	{
		Race->Tick(Parked, FVector::ZeroVector);
		++TicksToGo;
	}
	AddInfo(FString::Printf(TEXT("lights out after %d ticks (%.2f s)"), TicksToGo, TicksToGo * FBRRRace::TickSeconds));
	TestTrue(TEXT("countdown ends"), Race->HasStarted());

	const int32 RaceTicks = int32(120.0 / FBRRRace::TickSeconds);
	for (int32 T = 0; T < RaceTicks; ++T) Race->Tick(Parked, FVector::ZeroVector);

	int32 GhostLaps = 0;
	for (int32 R = 0; R < Race->NumRacers(); ++R)
	{
		const FBRRRacerStatus S = Race->Status(R);
		const double Moved = FVector::Dist(Start[R], Race->RacerTransform(R).GetLocation());
		FString Laps;
		for (const double L : S.LapTimes) Laps += FString::Printf(TEXT(" %.2f"), L);
		AddInfo(FString::Printf(TEXT("racer %d: place %d, laps %d%s, finished %d, moved %.0f cm"), R, S.Place, S.Lap,
			*Laps, S.bFinished, Moved));
		if (Race->IsGhost(R))
		{
			TestTrue(TEXT("ghost moved"), Moved > 1000.0);
			GhostLaps += S.Lap;
		}
	}
	TestTrue(TEXT("ghosts complete laps"), GhostLaps > 0);
	TestEqual(TEXT("parked player has no laps"), Race->Status(Player).Lap, 0);
	return true;
}

// DIRT1's destructible decor: the player's body set down on a piece knocks it over through the
// original's blocker probe, a UE knock does the same, and nothing else goes down.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRRaceWrecksTest, "BRR.Formats.Race.Dirt1Wrecks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRRaceWrecksTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FString Error;
	FBRRWorld World;
	if (!TestTrue(FString::Printf(TEXT("load DIRT1 %s"), *Error), FBRRWorld::Load(DataDir, TEXT("DIRT1"), {}, 1, World, Error))) return false;
	TUniquePtr<FBRRRace> Race = FBRRRace::Create(DataDir, TEXT("DIRT1"), TEXT("JEEP"), Error);
	if (!TestTrue(FString::Printf(TEXT("create %s"), *Error), Race.IsValid())) return false;
	if (!TestTrue(TEXT("DIRT1 has at least two destructibles"), World.Wrecks.Num() >= 2)) return false;

	for (const FBRRWreck& W : World.Wrecks) TestTrue(TEXT("standing at the start"), Race->WreckSeconds(W.Instance) < 0);

	// Park the player on the first piece.
	const FBRRWreck& Hit = World.Wrecks[0];
	FVector At = World.Placements[Hit.Instance].Location;
	At.Z = World.GroundHeightAt(At.X, At.Y);
	const uint32 Before = Race->WreckRevision();
	Race->Tick(FTransform(At), FVector::ZeroVector);
	TestTrue(TEXT("the probe knocks over the piece under the car"), Race->WreckSeconds(Hit.Instance) >= 0);
	TestNotEqual(TEXT("revision moves"), Race->WreckRevision(), Before);
	for (int32 T = 0; T < 10; ++T) Race->Tick(FTransform(At), FVector::ZeroVector);
	TestTrue(TEXT("its clock runs with the ticks"), FMath::IsNearlyEqual(Race->WreckSeconds(Hit.Instance), 10 * FBRRRace::TickSeconds, 1e-6));

	// Driving into a standing piece throws the car back off it, as any blocker does in the original.
	{
		const FBRRWreck& Bump = World.Wrecks[1];
		FVector Piece = World.Placements[Bump.Instance].Location;
		Piece.Z = World.GroundHeightAt(Piece.X, Piece.Y);
		const FVector In(2000.0, 0.0, 0.0); // 20 m/s along +X
		Race->Tick(FTransform(Piece - In.GetSafeNormal() * 150.0), In);
		FVector2D Out;
		if (TestTrue(TEXT("a moving car bounces off what it knocks over"), Race->ConsumePlayerBounce(Out)))
		{
			AddInfo(FString::Printf(TEXT("in %s cm/s, out %s cm/s"), *In.ToString(), *Out.ToString()));
			TestTrue(TEXT("the bounce loses speed"), Out.Size() < In.Size() + 1.0);
			TestFalse(TEXT("one bounce per knock"), Race->ConsumePlayerBounce(Out));
		}
	}

	const FBRRWreck& Knock = World.Wrecks.Last();
	Race->KnockOver(Knock.Instance);
	TestTrue(TEXT("a UE knock takes it down"), Race->WreckSeconds(Knock.Instance) >= 0);

	int32 Down = 0;
	for (const FBRRWreck& W : World.Wrecks) Down += Race->WreckSeconds(W.Instance) >= 0;
	AddInfo(FString::Printf(TEXT("%d of %d DIRT1 destructibles down"), Down, World.Wrecks.Num()));
	TestTrue(TEXT("the rest still stand"), Down <= 3);
	return true;
}

#endif
