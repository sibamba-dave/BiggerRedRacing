#include "BRRPhoto.h"

#include "BRRLook.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRPhoto, Log, All);

namespace
{
	// Frames for the denoised image to reach the screen once the last sample is in, and for the
	// screenshot to be written once requested.
	constexpr uint64 SettleFrames = 4;
}

ABRRPhoto::ABRRPhoto()
{
	PrimaryActorTick.bCanEverTick = true;
	SetTickableWhenPaused(true);
}

bool ABRRPhoto::Take(ABRRLook* InLook, int32 InSamples, const FString& InPath)
{
	if (IsBusy() || !InLook) return false;
	Look = InLook;
	Samples = FMath::Max(InSamples, 1);
	Path = InPath;

	UWorld* World = GetWorld();
	bWasPaused = UGameplayStatics::IsGamePaused(World);
	UGameplayStatics::SetGamePaused(World, true);
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (AHUD* Hud = PC->GetHUD())
		{
			bHudWasShown = Hud->bShowHUD;
			Hud->bShowHUD = false;
		}
	}
	if (IConsoleVariable* Spp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel"))) Spp->Set(Samples, ECVF_SetByCode);
	Look->Apply(/*bRemastered=*/true, /*bPathTracing=*/true, /*bFrameGeneration=*/false, /*bNativeResolution=*/true);
	Step = EStep::Gathering;
	StepFrame = GFrameCounter;
	UE_LOG(LogBRRPhoto, Display, TEXT("Photo: gathering %d samples per pixel"), Samples);
	return true;
}

void ABRRPhoto::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	switch (Step)
	{
	case EStep::Gathering:
		// A shader still compiling draws with a stand-in, and finishing it starts the samples
		// over, so the count starts once nothing is compiling.
		if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) StepFrame = GFrameCounter;
		else if (GFrameCounter >= StepFrame + uint64(Samples) + SettleFrames)
		{
			FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
			Step = EStep::Saving;
			StepFrame = GFrameCounter;
		}
		break;
	case EStep::Saving:
		if (GFrameCounter >= StepFrame + SettleFrames) Finish();
		break;
	default:
		break;
	}
}

void ABRRPhoto::Finish()
{
	UWorld* World = GetWorld();
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (AHUD* Hud = PC->GetHUD()) Hud->bShowHUD = bHudWasShown;
	}
	if (IConsoleVariable* Spp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel"))) Spp->Set(-1, ECVF_SetByCode);
	// The race puts its own look back (brr.Look and the rest) on its next tick.
	UGameplayStatics::SetGamePaused(World, bWasPaused);
	Saved = Path;
	Step = EStep::Idle;
	UE_LOG(LogBRRPhoto, Display, TEXT("Photo: saved %s"), *Path);
	if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 4.f, FColor::White, FString::Printf(TEXT("Photo saved: %s"), *FPaths::GetCleanFilename(Path)));
}
