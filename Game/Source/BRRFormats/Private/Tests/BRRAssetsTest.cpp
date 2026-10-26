#include "BRRArchive.h"
#include "BRRAssets.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRAssetsTest, "BRR.Formats.Assets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRAssetsTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FString Error;

	TArray<FColor> Palette;
	if (!TestTrue(TEXT("DEFAULT.COL"), BRR::LoadPalette(DataDir, TEXT("ALWAYS.WAP"), TEXT("DEFAULT.COL"), Palette, Error)))
	{
		AddError(Error);
		return false;
	}

	FBRRImage Panel;
	TestTrue(FString::Printf(TEXT("PANEL.TM %s"), *Error), BRR::LoadTexturePage(DataDir, TEXT("ALWAYS.WAP"), TEXT("PANEL.TM"), Palette, Panel, Error));
	TestEqual(TEXT("PANEL.TM width"), Panel.Width, 256);

	FBRRImage Screen;
	TestTrue(FString::Printf(TEXT("DIRT1 VIEW.RAW %s"), *Error), BRR::LoadScreen(DataDir, TEXT("DIRT1.LEV"), TEXT("VIEW.RAW"), Screen, Error));
	TestEqual(TEXT("screen size"), Screen.Pixels.Num(), 320 * 200);

	TArray<FBRRImage> Sprites;
	TestTrue(FString::Printf(TEXT("ALWAYS.SPR %s"), *Error), BRR::LoadSprites(DataDir, TEXT("ALWAYS.WAP"), TEXT("ALWAYS.SPR"), Palette, Sprites, Error));
	TestEqual(TEXT("ALWAYS.SPR count"), Sprites.Num(), 0x12);
	TArray<FBRRImage> Font;
	TestTrue(FString::Printf(TEXT("MICRO.FNT %s"), *Error), BRR::LoadSprites(DataDir, TEXT("ALWAYS.WAP"), TEXT("MICRO.FNT"), Palette, Font, Error));
	TestEqual(TEXT("MICRO.FNT glyphs"), Font.Num(), 96);

	FBRRTournament Tourn;
	TestTrue(FString::Printf(TEXT("TOURN.TXT %s"), *Error), BRR::LoadTournament(DataDir, Tourn, Error));
	TestEqual(TEXT("tournament tracks"), Tourn.Tracks.Num(), 24);
	TestTrue(TEXT("tournament rounds"), Tourn.Rounds.Num() > 0);
	if (Tourn.Tracks.Num() > 0)
	{
		const FBRRTournamentTrack& First = Tourn.Tracks[0];
		AddInfo(FString::Printf(TEXT("TOURN: %d tracks, %d single, %d rounds; first %s [%s] music CD track %d"), Tourn.Tracks.Num(),
			Tourn.SingleRaceOrder.Num(), Tourn.Rounds.Num(), *First.Level, *First.GlobeText, First.MusicCdTrack));
	}

	TestFalse(TEXT("missing member reports an error"), BRR::LoadScreen(DataDir, TEXT("DIRT1.LEV"), TEXT("NOPE.RAW"), Screen, Error));
	return true;
}

#endif
