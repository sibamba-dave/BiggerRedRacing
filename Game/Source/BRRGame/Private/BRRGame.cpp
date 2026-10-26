#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRGame, Log, All);

// Dev/regression switch: `-BRRShot=<seconds> [-BRRShotName=<file>]` saves a screenshot to
// Saved/Screenshots/<file> after the delay (long enough for shaders to compile), then quits. Used to
// compare frames against reference captures of the original without a human at the editor.
class FBRRGameModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		float Delay = 0.f;
		if (!FParse::Value(FCommandLine::Get(), TEXT("BRRShot="), Delay)) return;
		FString Name = TEXT("BRRShot.png");
		FParse::Value(FCommandLine::Get(), TEXT("BRRShotName="), Name);
		const FString Path = FPaths::Combine(FPaths::ScreenShotDir(), Name);

		double Elapsed = 0;
		bool bRequested = false;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([=](float DeltaTime) mutable
		{
			Elapsed += DeltaTime;
			if (!bRequested && Elapsed >= Delay)
			{
				FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
				UE_LOG(LogBRRGame, Display, TEXT("BRRShot: requested %s"), *Path);
				bRequested = true;
			}
			else if (bRequested && Elapsed >= Delay + 3.0)
			{
				FPlatformMisc::RequestExit(false, TEXT("BRRShot"));
				return false;
			}
			return true;
		}));
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FBRRGameModule, BRRGame, "BRRGame");
