#include "BRRFilm.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRFilm, Log, All);

namespace
{
	constexpr int32 SettleFrames = 12; // the image's history (TSR, Lumen, exposure) after a cut
}

ABRRFilm::ABRRFilm()
{
	PrimaryActorTick.bCanEverTick = false;
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
	UE_LOG(LogBRRFilm, Display, TEXT("%s: %d shots to film into %s"), *Level, Shots.Num(), *OutDir);
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
	const bool bCut = Frame == -SettleFrames - 1 || Frame < -SettleFrames;
	if (Frame < -SettleFrames) Frame = -SettleFrames;
	if (Frame == -SettleFrames) Anchor = P + F * 2600.0 + R * 900.0 + Up * 260.0;

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
		Frame = -SettleFrames - 1;
	}
}
