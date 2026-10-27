#include "BRRArchive.h"
#include "BRRProps.h"
#include "BRRWorld.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The lamp posts a level's decor holds (BRR::FindLamps): their heads must hang where the rebuilt
// lamp's glowing panel is - over the post's own box, well off the ground - so the race can hang a
// real light under each (ABRRRaceGameMode).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRFindLampsTest, "BRR.Formats.Props.FindLamps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRFindLampsTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	int32 Found = 0;
	for (const TCHAR* Level : { TEXT("DIRT1"), TEXT("CITY1"), TEXT("RUF1") })
	{
		FBRRWorld World;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("load %s: %s"), Level, *Error), FBRRWorld::Load(DataDir, Level, {}, 1, World, Error))) return false;
		const TArray<BRR::FBRRLampHead> Lamps = BRR::FindLamps(World);
		Found += Lamps.Num();
		for (const BRR::FBRRLampHead& Lamp : Lamps)
		{
			// Near its own placement, and hanging high: a lamp stands over 2 m tall.
			float Nearest = 1e9f;
			float PlaceZ = 0;
			for (const FBRRPlacement& P : World.Placements)
			{
				const float D = FVector3f::Dist2D(FVector3f(P.Location), Lamp.Head);
				if (D < Nearest) { Nearest = D; PlaceZ = float(P.Location.Z); }
			}
			TestTrue(FString::Printf(TEXT("%s lamp head near its post (%.0f cm off)"), Level, Nearest), Nearest < 2000.f);
			TestTrue(FString::Printf(TEXT("%s lamp head off the ground"), Level), Lamp.Head.Z > PlaceZ + 150.f);
		}
		AddInfo(FString::Printf(TEXT("%s: %d lamp heads"), Level, Lamps.Num()));
	}
	TestTrue(TEXT("some level holds lamp posts"), Found > 0);
	return true;
}

#endif
