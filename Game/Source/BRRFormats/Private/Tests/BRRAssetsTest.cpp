#include "BRRArchive.h"
#include "BRRAssets.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

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

// The in-race HUD artwork (GAMESVGA.SPR, SML_ALPH.SPR) carries palette indices. On the original's
// 8-bit screen only the level's palette is loaded during a race, so the question is how far the
// level palettes stray from DEFAULT.COL at the indices that artwork uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRHudPaletteTest, "BRR.Formats.HudPalette",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRHudPaletteTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FString Error;
	TArray<FColor> Default;
	if (!TestTrue(TEXT("DEFAULT.COL"), BRR::LoadPalette(DataDir, TEXT("ALWAYS.WAP"), TEXT("DEFAULT.COL"), Default, Error))) return false;
	TArray<FString> Levels;
	IFileManager::Get().FindFiles(Levels, *FPaths::Combine(DataDir, TEXT("*.LEV")), true, false);
	Levels.Sort();
	for (const FString& Lev : Levels)
	{
		TArray<FColor> Level;
		if (!BRR::LoadPalette(DataDir, Lev, TEXT("MAP.COL"), Level, Error) && !BRR::LoadPalette(DataDir, Lev, TEXT("1.COL"), Level, Error)) continue;
		for (const TPair<const TCHAR*, const TCHAR*>& Sheet : { TPair<const TCHAR*, const TCHAR*>(TEXT("INGAME.WAP"), TEXT("GAMESVGA.SPR")),
			TPair<const TCHAR*, const TCHAR*>(TEXT("ALWAYS.WAP"), TEXT("SML_ALPH.SPR")) })
		{
			TArray<FBRRImage> A, B;
			if (!BRR::LoadSprites(DataDir, Sheet.Key, Sheet.Value, Default, A, Error) || !BRR::LoadSprites(DataDir, Sheet.Key, Sheet.Value, Level, B, Error)) continue;
			int64 Opaque = 0, Same = 0, Close = 0;
			int32 Worst = 0;
			for (int32 S = 0; S < A.Num() && S < B.Num(); ++S)
			{
				// Only what the race HUD draws (ABRRRaceHUD): digits, ordinals, racer dots, the
				// banners, the small digits.
				const bool bRaceHud = FCString::Strcmp(Sheet.Value, TEXT("SML_ALPH.SPR")) == 0 || S <= 21 || (S >= 27 && S <= 36) || S == 40;
				if (!bRaceHud) continue;
				for (int32 P = 0; P < A[S].Pixels.Num(); ++P)
				{
					const FColor X = A[S].Pixels[P], Y = B[S].Pixels[P];
					if (X.A == 0) continue;
					++Opaque;
					const int32 D = FMath::Max3(FMath::Abs(X.R - Y.R), FMath::Abs(X.G - Y.G), FMath::Abs(X.B - Y.B));
					Same += D == 0;
					Close += D <= 8;
					Worst = FMath::Max(Worst, D);
				}
			}
			AddInfo(FString::Printf(TEXT("%s %s: %lld px, %.1f%% same colour, %.1f%% within 8, worst %d"), *FPaths::GetBaseFilename(Lev),
				Sheet.Value, Opaque, 100.0 * Same / FMath::Max<int64>(Opaque, 1), 100.0 * Close / FMath::Max<int64>(Opaque, 1), Worst));
		}
	}
	return true;
}

#endif
