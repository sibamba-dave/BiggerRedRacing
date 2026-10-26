#include "BRRArchive.h"
#include "BRRRace.h"

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

#endif
