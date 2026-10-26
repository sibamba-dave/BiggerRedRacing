#include "BRRArchive.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Decodes every archive in the original data folder. Expected totals come from the first-pass
// reverse-engineering survey of the shipped CD (78 archives, 2777 members).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRArchiveDecodeAllTest, "BRR.Formats.Archive.DecodeAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRArchiveDecodeAllTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}

	static const TCHAR* Extensions[] = { TEXT("LEV"), TEXT("ENV"), TEXT("VHC"), TEXT("SCN"), TEXT("WAP"), TEXT("TEA") };
	int32 ArchiveCount = 0;
	int32 MemberCount = 0;
	for (const TCHAR* Ext : Extensions)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(DataDir, FString(TEXT("*.")) + Ext), true, false);
		for (const FString& File : Files)
		{
			FBRRArchive Archive;
			FString Error;
			const bool bLoaded = FBRRArchive::Load(FPaths::Combine(DataDir, File), Archive, Error);
			if (!TestTrue(FString::Printf(TEXT("%s decodes (%s)"), *File, *Error), bLoaded))
			{
				continue;
			}
			++ArchiveCount;
			MemberCount += Archive.Entries.Num();
		}
	}
	// The whole CD's RACING folder (Tools/brr_cd.py restores what a partial rip lacks: BYESCN.SCN).
	TestEqual(TEXT("archive count"), ArchiveCount, 79);
	TestEqual(TEXT("member count"), MemberCount, 2779);
	return true;
}

#endif
