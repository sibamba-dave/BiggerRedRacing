#include "BRRFilm.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRFilm, Log, All);

namespace
{
	constexpr int32 CutSettleFrames = 12; // the image's history (TSR, Lumen, exposure) after a cut
}

ABRRFilm::ABRRFilm()
{
	PrimaryActorTick.bCanEverTick = true; // only the path-traced gather uses it, under the pause
	SetTickableWhenPaused(true);
}

bool ABRRFilm::Load(const FString& PlanPath, const FString& InOutDir, const FString& Level)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *PlanPath))
	{
		UE_LOG(LogBRRFilm, Error, TEXT("No plan at %s"), *PlanPath);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root) return false;
	for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("shots")))
	{
		const TSharedPtr<FJsonObject> S = V->AsObject();
		FString Circuit;
		if (!S || !S->TryGetStringField(TEXT("circuit"), Circuit) || !Circuit.Equals(Level, ESearchCase::IgnoreCase)) continue;
		FShot& Shot = Shots.AddDefaulted_GetRef();
		Shot.Id = int32(S->GetNumberField(TEXT("id")));
		Shot.Frames = int32(S->GetNumberField(TEXT("end")) - S->GetNumberField(TEXT("start"))) * 2;
		Shot.Camera = S->GetStringField(TEXT("camera"));
		Shot.Racer = int32(S->GetNumberField(TEXT("racer")));
	}
	OutDir = InOutDir;
	// Path-traced filming: every saved frame converges on a frozen scene before time takes its
	// next 30 Hz step - the offline route to re-rendering the videos at photo quality.
	if (FParse::Value(FCommandLine::Get(), TEXT("BRRFilmPT="), PTSamples) && PTSamples > 0)
	{
		PTSamples = FMath::Clamp(PTSamples, 4, 1024);
		if (IConsoleVariable* PT = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.PathTracing"))) PT->Set(1, ECVF_SetByCode);
		if (IConsoleVariable* Spp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel"))) Spp->Set(PTSamples, ECVF_SetByCode);
		// Native resolution, as the photo holds it: under an upscaler's jitter the accumulation
		// never settles.
		if (IConsoleVariable* Native = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.DLAA"))) Native->Set(1, ECVF_SetByCode);
		// Something nudges a material parameter every frame even while the world stands paused,
		// and each nudge would restart the accumulation: filming ignores those (the scene is held
		// still by the pause; the parameters that matter cannot move).
		if (IConsoleVariable* Inv = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.InvalidateOnMaterialParameterChange"))) Inv->Set(0, ECVF_SetByCode);
	}
	UE_LOG(LogBRRFilm, Display, TEXT("%s: %d shots to film into %s%s"), *Level, Shots.Num(), *OutDir,
		PTSamples > 0 ? *FString::Printf(TEXT(" (path traced, %d samples a frame)"), PTSamples) : TEXT(""));
	return !Shots.IsEmpty();
}

void ABRRFilm::Update(const TArray<FTransform>& Racers, double RaceSeconds, ACameraActor* Camera)
{
	if (bDone || !Camera || Racers.IsEmpty()) return;
	if (Current >= Shots.Num())
	{
		bDone = true;
		UE_LOG(LogBRRFilm, Display, TEXT("Filmed %d shots"), Shots.Num());
		FPlatformMisc::RequestExit(false, TEXT("BRRFilm"));
		return;
	}
	if (RaceSeconds < StartAt) return;
	const FShot& Shot = Shots[Current];
	const FTransform& Car = Racers[FMath::Clamp(Shot.Racer, 0, Racers.Num() - 1)];
	const FVector P = Car.GetLocation();
	const FVector F = Car.GetUnitAxis(EAxis::X).GetSafeNormal2D();
	const FVector R = FVector(-F.Y, F.X, 0);
	const FVector Up = FVector::UpVector;
	const bool bCut = Frame == -CutSettleFrames - 1 || Frame < -CutSettleFrames;
	if (Frame < -CutSettleFrames) Frame = -CutSettleFrames;
	if (Frame == -CutSettleFrames) Anchor = P + F * 2600.0 + R * 900.0 + Up * 260.0;

	FVector WantEye, WantLook;
	float Fov = 62.f;
	if (Shot.Camera == TEXT("side"))
	{
		WantEye = P + R * 650.0 + Up * 140.0 - F * 150.0;
		WantLook = P + F * 250.0 + Up * 60.0;
	}
	else if (Shot.Camera == TEXT("trackside"))
	{
		WantEye = Anchor;
		WantLook = P + Up * 60.0;
		Fov = 45.f;
	}
	else if (Shot.Camera == TEXT("high"))
	{
		WantEye = P - F * 1600.0 + Up * 950.0;
		WantLook = P + F * 900.0;
	}
	else if (Shot.Camera == TEXT("front"))
	{
		WantEye = P + F * 1000.0 + Up * 170.0 + R * 120.0;
		WantLook = P + Up * 80.0;
	}
	else // chase
	{
		WantEye = P - F * 720.0 + Up * 230.0;
		WantLook = P + F * 450.0 + Up * 60.0;
	}
	// Held a little behind its target, so the ghosts' small jolts do not shake the picture.
	const double Follow = bCut || Shot.Camera == TEXT("trackside") ? 1.0 : 0.35;
	Eye = FMath::Lerp(Eye, WantEye, Follow);
	Look = FMath::Lerp(Look, WantLook, bCut ? 1.0 : 0.5);
	Camera->SetActorLocationAndRotation(Eye, (Look - Eye).Rotation());
	if (UCameraComponent* Lens = Camera->GetCameraComponent())
	{
		Lens->bConstrainAspectRatio = false;
		Lens->SetFieldOfView(Fov);
	}

	if (Frame >= 0 && PTSamples > 0)
	{
		// The camera is placed for this frame: pause the game - the scene truly stands still, as
		// the photo mode holds it - and gather, with frames on top for the denoised image to land.
		Gather = PTSamples + 6;
		UGameplayStatics::SetGamePaused(GetWorld(), true);
		return;
	}
	if (Frame >= 0)
	{
		const FString Path = OutDir / FString::Printf(TEXT("shot_%03d/%04d.png"), Shot.Id, Frame);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
	}
	++Frame;
	if (Frame >= Shot.Frames)
	{
		++Current;
		Frame = -CutSettleFrames - 1;
	}
}

void ABRRFilm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (PTSamples <= 0 || Gather <= 0 || !UGameplayStatics::IsGamePaused(GetWorld())) return;
	// A shader still compiling draws with a stand-in and restarts the samples (as the photo
	// waits): the gather only counts once nothing is compiling.
	if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
	{
		Gather = PTSamples + 6;
		return;
	}
	--Gather;
	// The screenshot resolves on the frame AFTER its request: it is asked for two held frames
	// before the pause lifts, so it captures the converged image, not a fresh accumulation.
	if (Gather == 2 && Current < Shots.Num() && Frame >= 0)
	{
		const FString Path = OutDir / FString::Printf(TEXT("shot_%03d/%04d.png"), Shots[Current].Id, Frame);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		++Frame;
		if (Frame >= Shots[Current].Frames)
		{
			++Current;
			Frame = -CutSettleFrames - 1;
		}
	}
	if (Gather > 0) return;
	UGameplayStatics::SetGamePaused(GetWorld(), false); // one 30 Hz step runs, then the next gather
}
