#include "BRRRaceHUD.h"

#include "BRRArchive.h"
#include "BRRRaceGameMode.h"
#include "BRRVehiclePawn.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRHUD, Log, All);

namespace
{
	// GAMESVGA.SPR glyphs (ORR formats/sprite.h, game_spr).
	constexpr int32 Digit0 = 0, OrdinalSt = 10, Dot0 = 14, WrongWay = 20, FinalLap = 21, SmallDigit0 = 27, MissedCheck = 40;

	// The original's speedometer calibration (ORR race_board): its dials read 120 at a forward
	// speed of 35000 (1/512 units per 28 ms tick).
	int32 DialKmh(double CmPerSecond)
	{
		const double Vfwd = CmPerSecond / BRR::CmPerUnit * 512.0 * 0.028;
		return FMath::Abs(FMath::RoundToInt32(Vfwd * 120.0 / 35000.0));
	}
}

UTexture2D* ABRRRaceHUD::MakeTexture(const FBRRImage& Image)
{
	if (Image.Width <= 0 || Image.Height <= 0) return nullptr;
	UTexture2D* Texture = UTexture2D::CreateTransient(Image.Width, Image.Height, PF_B8G8R8A8);
	Texture->Filter = TF_Nearest;
	Texture->SRGB = true;
	void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Image.Pixels.GetData(), Image.Pixels.Num() * sizeof(FColor));
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

bool ABRRRaceHUD::LoadArtwork()
{
	bTriedLoad = true;
	const ABRRRaceGameMode* Mode = GetWorld()->GetAuthGameMode<ABRRRaceGameMode>();
	const FString DataDir = BRR::GetDataDir();
	if (!Mode || DataDir.IsEmpty()) return false;

	FString Error;
	TArray<FColor> Palette, LevelPalette;
	TArray<FBRRImage> Images;
	if (!BRR::LoadPalette(DataDir, TEXT("ALWAYS.WAP"), TEXT("DEFAULT.COL"), Palette, Error))
	{
		UE_LOG(LogBRRHUD, Warning, TEXT("No HUD palette: %s"), *Error);
		return false;
	}
	if (BRR::LoadSprites(DataDir, TEXT("INGAME.WAP"), TEXT("GAMESVGA.SPR"), Palette, Images, Error))
		for (const FBRRImage& I : Images) Game.Add(MakeTexture(I));
	if (BRR::LoadSprites(DataDir, TEXT("ALWAYS.WAP"), TEXT("SML_ALPH.SPR"), Palette, Images, Error))
		for (const FBRRImage& I : Images) Alphabet.Add(MakeTexture(I));

	const FString Lev = Mode->GetLevelName() + TEXT(".LEV");
	if ((BRR::LoadPalette(DataDir, Lev, TEXT("MAP.COL"), LevelPalette, Error) || BRR::LoadPalette(DataDir, Lev, TEXT("1.COL"), LevelPalette, Error))
		&& BRR::LoadTopMap(DataDir, Mode->GetLevelName(), LevelPalette, TopMap, Error))
	{
		MapTexture = MakeTexture(TopMap.Image);
	}
	UE_LOG(LogBRRHUD, Display, TEXT("HUD artwork: %d game sprites, %d letters, map %s"), Game.Num(), Alphabet.Num(),
		MapTexture ? TEXT("yes") : TEXT("no"));
	return Game.Num() > MissedCheck;
}

float ABRRRaceHUD::SpriteWidth(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index) const
{
	return Sheet.IsValidIndex(Index) && Sheet[Index] ? Sheet[Index]->GetSizeX() : 0.f;
}

float ABRRRaceHUD::SpriteHeight(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index) const
{
	return Sheet.IsValidIndex(Index) && Sheet[Index] ? Sheet[Index]->GetSizeY() : 0.f;
}

float ABRRRaceHUD::Sprite(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index, float X, float Y, float Scale, float Alpha)
{
	if (!Sheet.IsValidIndex(Index) || !Sheet[Index]) return X;
	UTexture2D* T = Sheet[Index];
	FCanvasTileItem Tile(FVector2D(FMath::RoundToFloat(X), FMath::RoundToFloat(Y)), T->GetResource(),
		FVector2D(T->GetSizeX() * Scale, T->GetSizeY() * Scale), FLinearColor(1, 1, 1, Alpha));
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
	return X + T->GetSizeX() * Scale;
}

float ABRRRaceHUD::TextWidth(const FString& S, float Scale) const
{
	float W = 0;
	for (const TCHAR C : S) W += (SpriteWidth(Alphabet, BRR::InterfaceGlyph(C)) + 1.f) * Scale;
	return W;
}

float ABRRRaceHUD::Text(const FString& S, float X, float Y, float Scale, const FLinearColor& Tint)
{
	for (const TCHAR C : S)
	{
		const int32 G = BRR::InterfaceGlyph(C);
		if (!Alphabet.IsValidIndex(G) || !Alphabet[G]) continue;
		UTexture2D* T = Alphabet[G];
		FCanvasTileItem Tile(FVector2D(FMath::RoundToFloat(X), FMath::RoundToFloat(Y)), T->GetResource(),
			FVector2D(T->GetSizeX() * Scale, T->GetSizeY() * Scale), Tint);
		Tile.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Tile);
		X += (T->GetSizeX() + 1.f) * Scale;
	}
	return X;
}

float ABRRRaceHUD::NumberWidth(int32 Value, float Scale) const
{
	const FString Digits = FString::FromInt(FMath::Max(Value, 0));
	float W = 0;
	for (const TCHAR C : Digits) W += SpriteWidth(Game, SmallDigit0 + (C - '0')) * Scale;
	return W;
}

float ABRRRaceHUD::Number(int32 Value, float X, float Y, float Scale)
{
	for (const TCHAR C : FString::FromInt(FMath::Max(Value, 0))) X = Sprite(Game, SmallDigit0 + (C - '0'), X, Y, Scale);
	return X;
}

float ABRRRaceHUD::ClockWidth(float Scale) const
{
	return NumberWidth(0, Scale) * 6.f + TextWidth(TEXT(":."), Scale * 4.f);
}

float ABRRRaceHUD::Clock(double Seconds, float X, float Y, float Scale)
{
	const int32 Hundredths = FMath::FloorToInt32(Seconds * 100.0);
	const int32 M = Hundredths / 6000, S = (Hundredths / 100) % 60, H = Hundredths % 100;
	const float DigitH = SpriteHeight(Game, SmallDigit0) * Scale;
	const float SepScale = Scale * 4.f;
	const float SepY = Y + DigitH - SpriteHeight(Alphabet, BRR::InterfaceGlyph(':')) * SepScale;
	X = Number(M, X, Y, Scale);
	X = Text(TEXT(":"), X, SepY, SepScale);
	X = Number(S / 10, X, Y, Scale);
	X = Number(S % 10, X, Y, Scale);
	X = Text(TEXT("."), X, SepY, SepScale);
	X = Number(H / 10, X, Y, Scale);
	return Number(H % 10, X, Y, Scale);
}

void ABRRRaceHUD::Centred(int32 Index, float CX, float Y, float Scale)
{
	Sprite(Game, Index, CX - SpriteWidth(Game, Index) * Scale * 0.5f, Y, Scale);
}

void ABRRRaceHUD::DrawHUD()
{
	Super::DrawHUD();
	const ABRRRaceGameMode* Mode = GetWorld()->GetAuthGameMode<ABRRRaceGameMode>();
	const FBRRRace* Race = Mode ? Mode->GetRace() : nullptr;
	if (!Race || !Canvas) return;
	if (!bTriedLoad) LoadArtwork();
	if (Game.Num() <= MissedCheck) return;

	const float W = Canvas->ClipX, H = Canvas->ClipY;
	const float K = H / 480.f;                         // the SVGA artwork is drawn for 480 lines
	const float Margin = 16.f * K;
	const float Fs = FMath::Max(1.f, FMath::RoundToFloat(1.4f * K)); // SML_ALPH, whole-pixel magnification
	const float Ds = 0.36f * K;                        // the 42-pixel digits
	const float DigitH = SpriteHeight(Game, SmallDigit0) * Ds;
	const float Line = DigitH + 5.f * K;
	const float LabelDy = (DigitH - SpriteHeight(Alphabet, BRR::InterfaceGlyph('A')) * Fs) * 0.5f;
	const FBRRRacerStatus Me = Race->Status(Race->PlayerIndex());

	// Top right: lap, clock, last lap.
	{
		const int32 Shown = FMath::Min(Me.Lap + 1, Race->Laps());
		const float LapW = TextWidth(TEXT("LAP "), Fs) + NumberWidth(Shown, Ds) + TextWidth(TEXT("/"), Fs) + NumberWidth(Race->Laps(), Ds);
		float Pen = W - Margin - LapW;
		Pen = Text(TEXT("LAP "), Pen, Margin + LabelDy, Fs);
		Pen = Number(Shown, Pen, Margin, Ds);
		Pen = Text(TEXT("/"), Pen, Margin + LabelDy, Fs);
		Number(Race->Laps(), Pen, Margin, Ds);
		Clock(Race->RaceTime(), W - Margin - ClockWidth(Ds), Margin + Line, Ds);
		if (!Me.LapTimes.IsEmpty())
		{
			const float Cw = ClockWidth(Ds * 0.8f);
			Text(TEXT("LAST "), W - Margin - Cw - TextWidth(TEXT("LAST "), Fs), Margin + Line * 2 + LabelDy, Fs, FLinearColor(0.7f, 0.8f, 1.f));
			Clock(Me.LapTimes.Last(), W - Margin - Cw, Margin + Line * 2, Ds * 0.8f);
		}
	}

	// Top left: the placing in the game's large digits and ordinal, the mini-map under it.
	float PlaceBottom = Margin;
	{
		const int32 Place = FMath::Clamp(Me.Place, 1, 9);
		const int32 Ordinal = OrdinalSt + (Place == 1 ? 0 : Place == 2 ? 1 : Place == 3 ? 2 : 3);
		const float Ps = 0.9f * K;
		const float Pen = Sprite(Game, Digit0 + Place, Margin, Margin, Ps);
		Sprite(Game, Ordinal, Pen, Margin, Ps);
		PlaceBottom = Margin + SpriteHeight(Game, Digit0 + Place) * Ps;
	}
	if (MapTexture)
	{
		const float Ms = FMath::Max(1.f, FMath::RoundToFloat(K));
		const float Mx = Margin, My = PlaceBottom + 6.f * K;
		FCanvasTileItem Map(FVector2D(Mx, My), MapTexture->GetResource(),
			FVector2D(MapTexture->GetSizeX() * Ms, MapTexture->GetSizeY() * Ms), FLinearColor(1, 1, 1, 0.85f));
		Map.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Map);
		// Every racer's dot in its own colour, as the original draws them; the player's last.
		auto Dot = [&](int32 R)
		{
			const int32 Glyph = Dot0 + FMath::Clamp(Race->ColourOf(R) - 1, 0, 5);
			const FVector2D Px = TopMap.ToPixel(Race->RacerTransform(R).GetLocation());
			Sprite(Game, Glyph, Mx + Px.X * Ms - SpriteWidth(Game, Glyph) * Ms * 0.5f, My + Px.Y * Ms - SpriteHeight(Game, Glyph) * Ms * 0.5f, Ms);
		};
		for (int32 R = 0; R < Race->NumRacers(); ++R)
			if (R != Race->PlayerIndex()) Dot(R);
		Dot(Race->PlayerIndex());
	}

	// Bottom right: the speedometer reading.
	if (const ABRRVehiclePawn* Car = Mode->GetPlayerCar())
	{
		const int32 Kmh = DialKmh(Car->GetForwardSpeed());
		const float UnitW = TextWidth(TEXT(" KM/H"), Fs);
		const float Sy = H - Margin - DigitH;
		Number(Kmh, W - Margin - UnitW - NumberWidth(Kmh, Ds), Sy, Ds);
		Text(TEXT(" KM/H"), W - Margin - UnitW, Sy + LabelDy, Fs);
	}

	// Centre: the banners the game has artwork for.
	const float CX = W * 0.5f;
	if (Me.Lap + 1 == Race->Laps() && !Me.bFinished)
	{
		if (FinalLapShownAt != Me.Lap)
		{
			FinalLapShownAt = Me.Lap;
			FinalLapStart = Race->RaceTime();
		}
		if (Race->RaceTime() - FinalLapStart < 5.0) Centred(FinalLap, CX, H * 0.12f, K);
	}
	if (Me.bWrongWay) Centred(WrongWay, CX, H * 0.30f, K);
	else if (Me.bMissedCheckpoint) Centred(MissedCheck, CX, H * 0.30f, K);
	if (Me.bFinished)
	{
		const int32 Place = FMath::Clamp(Me.Place, 1, 9);
		const float Ps = 1.5f * K;
		const float Wd = (SpriteWidth(Game, Digit0 + Place) + SpriteWidth(Game, OrdinalSt + 3)) * Ps;
		const float Pen = Sprite(Game, Digit0 + Place, CX - Wd * 0.5f, H * 0.25f, Ps);
		Sprite(Game, OrdinalSt + (Place == 1 ? 0 : Place == 2 ? 1 : Place == 3 ? 2 : 3), Pen, H * 0.25f, Ps);
	}
}
