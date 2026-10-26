#include "BRRRaceHUD.h"

#include "BRRRaceGameMode.h"
#include "BRRVehiclePawn.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"

namespace
{
	FString FormatTime(double Seconds)
	{
		const int32 Minutes = int32(Seconds / 60.0);
		return FString::Printf(TEXT("%d:%05.2f"), Minutes, Seconds - Minutes * 60.0);
	}
}

void ABRRRaceHUD::DrawHUD()
{
	Super::DrawHUD();
	const ABRRRaceGameMode* Mode = GetWorld()->GetAuthGameMode<ABRRRaceGameMode>();
	const FBRRRace* Race = Mode ? Mode->GetRace() : nullptr;
	if (!Race || !Canvas) return;

	UFont* Font = GEngine->GetLargeFont();
	const float Scale = Canvas->ClipY / 720.f;
	auto Text = [&](const FString& S, float X, float Y, const FLinearColor& Colour, float Size = 1.f, bool bCentre = false)
	{
		float W = 0, H = 0;
		GetTextSize(S, W, H, Font, Scale * Size);
		DrawText(S, Colour, bCentre ? X - W * 0.5f : X, Y, Font, Scale * Size);
	};

	const FBRRRacerStatus S = Race->Status(Race->PlayerIndex());
	const float Left = 24.f * Scale, Top = 20.f * Scale, Line = 28.f * Scale;
	Text(FString::Printf(TEXT("LAP %d/%d"), FMath::Min(S.Lap + 1, Race->Laps()), Race->Laps()), Left, Top, FLinearColor::Yellow, 1.4f);
	Text(FString::Printf(TEXT("POS %d/%d"), S.Place, Race->NumRacers()), Left, Top + Line * 1.4f, FLinearColor::White, 1.4f);
	Text(FormatTime(Race->RaceTime()), Left, Top + Line * 2.8f, FLinearColor::White);
	for (int32 L = 0; L < S.LapTimes.Num(); ++L)
	{
		Text(FString::Printf(TEXT("L%d %s"), L + 1, *FormatTime(S.LapTimes[L])), Left, Top + Line * (3.8f + L), FLinearColor(0.7f, 0.9f, 1.f));
	}
	if (const ABRRVehiclePawn* Car = Mode->GetPlayerCar())
	{
		Text(FString::Printf(TEXT("%3.0f km/h"), FMath::Abs(Car->GetForwardSpeed()) * 0.036), Canvas->ClipX - 200.f * Scale,
			Canvas->ClipY - 50.f * Scale, FLinearColor::White, 1.4f);
	}

	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 0.35f;
	const int32 Count = Race->CountdownNumber();
	if (Count > 0) Text(FString::FromInt(Count), CX, CY, FLinearColor::Red, 8.f, true);
	else if (Count == 0) Text(TEXT("GO!"), CX, CY, FLinearColor::Green, 8.f, true);
	if (S.bFinished) Text(FString::Printf(TEXT("FINISHED  %d%s"), S.Place, S.Place == 1 ? TEXT("st") : S.Place == 2 ? TEXT("nd") : S.Place == 3 ? TEXT("rd") : TEXT("th")), CX, CY, FLinearColor::Yellow, 3.f, true);
	else if (S.bWrongWay) Text(TEXT("WRONG WAY"), CX, CY, FLinearColor::Red, 3.f, true);
	else if (S.bMissedCheckpoint) Text(TEXT("MISSED CHECKPOINT"), CX, CY + 80.f * Scale, FLinearColor(1.f, 0.5f, 0.f), 2.f, true);
}
