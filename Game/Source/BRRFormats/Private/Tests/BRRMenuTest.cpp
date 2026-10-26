#include "BRRArchive.h"
#include "BRRAssets.h"
#include "BRRScn.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The front end's data: every menu screen the game uses loads with its picture, boxes and strings, and the
// strings lay out where the screens expect them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRMenuScreensTest, "BRR.Formats.Menus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRMenuScreensTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FBRRText Text;
	TestTrue(TEXT("TOURN.ENG loads"), Text.Load(DataDir, TEXT("ENG")));

	struct FCase { const TCHAR* Name; int32 Boxes; int32 Texts; const TCHAR* PicArchive; const TCHAR* PicMember; };
	for (const FCase& C : { FCase{ TEXT("CONFSCN"), 5, 5, nullptr, nullptr }, FCase{ TEXT("RESULSCN"), 2, 12, nullptr, nullptr },
		FCase{ TEXT("TRAKSCN"), 4, 4, TEXT("DIRT1.LEV"), TEXT("VIEW.RAW") }, FCase{ TEXT("GLOBESCN"), 10, 12, nullptr, nullptr },
		FCase{ TEXT("OPTSCN"), 7, 5, nullptr, nullptr }, FCase{ TEXT("SOUNDSCN"), 23, 4, nullptr, nullptr } })
	{
		FBRRScn Scn;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s loads: %s"), C.Name, *Error),
			BRR::LoadScn(DataDir, C.Name, TEXT("ENG"), Scn, Error, C.PicArchive ? C.PicArchive : TEXT(""), C.PicMember ? C.PicMember : TEXT(""))))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s picture"), C.Name), Scn.Background.Width, 320);
		TestEqual(FString::Printf(TEXT("%s boxes"), C.Name), Scn.Boxes.Num(), C.Boxes);
		TestEqual(FString::Printf(TEXT("%s strings"), C.Name), Scn.Texts.Num(), C.Texts);
		for (const FBRRScnText& T : Scn.Texts)
		{
			TestFalse(FString::Printf(TEXT("%s: %s has text"), C.Name, *T.Key), Text.Text(T.Key).IsEmpty());
		}
	}

	const TArray<FBRRTextRun> Runs = Text.Layout(TEXT("CONFIG_SINGLE"));
	TestTrue(TEXT("CONFIG_SINGLE is placed"), Runs.Num() > 0 && Runs[0].X >= 0 && Runs[0].Y >= 0);
	TestTrue(TEXT("DIRT1 offers cars"), BRR::LevelCars(DataDir, TEXT("DIRT1")).Num() >= 2);
	return true;
}

#endif
