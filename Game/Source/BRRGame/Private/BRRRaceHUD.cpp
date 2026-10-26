#include "BRRRaceHUD.h"

#include "BRRArchive.h"
#include "BRRRaceGameMode.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRHUD, Log, All);

namespace
{
	// GAMESVGA.SPR glyphs (ORR formats/sprite.h, game_spr).
	constexpr int32 Digit0 = 0, OrdinalSt = 10, Dot0 = 14, WrongWay = 20, FinalLap = 21, Pip = 37, PipSpent = 38, Bar = 39, MissedCheck = 40;
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

	// Everything on the race screen goes through the level's palette: the original's VGA holds
	// one 256-colour palette and a race loads the level's. Checked against the original on
	// WATER1 (Tools/dosbox-x): its magenta map dot is the level's 38,00,39, not DEFAULT.COL's
	// 3f,00,3f, which ORR and this HUD had used.
	FString Error;
	TArray<FColor> LevelPalette;
	TArray<FBRRImage> Images;
	const FString Lev = Mode->GetLevelName() + TEXT(".LEV");
	if (!BRR::LoadPalette(DataDir, Lev, TEXT("MAP.COL"), LevelPalette, Error) && !BRR::LoadPalette(DataDir, Lev, TEXT("1.COL"), LevelPalette, Error))
	{
		UE_LOG(LogBRRHUD, Warning, TEXT("No level palette for the HUD: %s"), *Error);
		return false;
	}
	if (BRR::LoadSprites(DataDir, TEXT("INGAME.WAP"), TEXT("GAMESVGA.SPR"), LevelPalette, Images, Error))
	{
		for (const FBRRImage& I : Images)
		{
			Game.Add(MakeTexture(I));
			// Where each glyph's drawing starts inside its image, so the original's measured
			// positions can be met exactly.
			FIntPoint Inset(I.Width, I.Height);
			for (int32 Y = 0; Y < I.Height; ++Y)
				for (int32 X = 0; X < I.Width; ++X)
					if (I.Pixels[Y * I.Width + X].A) Inset = FIntPoint(FMath::Min(Inset.X, X), FMath::Min(Inset.Y, Y));
			GameInset.Add(Inset);
		}
		// The bar is two flat rectangles in palette colours the artwork already has: its frame
		// is the darkest of GAMESVGA glyph 39 and its fill the pip's brightest red. Painted from
		// one texel of each, so they reach the screen as exactly as the sprites.
		auto Texel = [&Images](int32 Glyph, TFunctionRef<int32(const FColor&)> Score) -> FVector2D
		{
			if (!Images.IsValidIndex(Glyph)) return FVector2D::ZeroVector;
			const FBRRImage& I = Images[Glyph];
			int32 Best = -1;
			for (int32 P = 0; P < I.Pixels.Num(); ++P)
				if (I.Pixels[P].A && (Best < 0 || Score(I.Pixels[P]) > Score(I.Pixels[Best]))) Best = P;
			return Best < 0 ? FVector2D::ZeroVector : FVector2D((Best % I.Width + 0.5) / I.Width, (Best / I.Width + 0.5) / I.Height);
		};
		FillTexel = Texel(Pip, [](const FColor& C) { return int32(C.R) - C.G - C.B; });
		FrameTexel = Texel(Bar, [](const FColor& C) { return -int32(C.R) - C.G - C.B; });
	}
	if (BRR::LoadSprites(DataDir, TEXT("ALWAYS.WAP"), TEXT("SML_ALPH.SPR"), LevelPalette, Images, Error))
		for (const FBRRImage& I : Images) Alphabet.Add(MakeTexture(I));
	if (BRR::LoadTopMap(DataDir, Mode->GetLevelName(), LevelPalette, TopMap, Error))
	{
		MapTexture = MakeTexture(TopMap.Image);
		MapInset = FIntPoint(TopMap.Image.Width, TopMap.Image.Height);
		for (int32 Y = 0; Y < TopMap.Image.Height; ++Y)
		{
			for (int32 X = 0; X < TopMap.Image.Width; ++X)
			{
				if (TopMap.Image.Pixels[Y * TopMap.Image.Width + X].A == 0) continue;
				MapInset.X = FMath::Min(MapInset.X, X);
				MapInset.Y = FMath::Min(MapInset.Y, Y);
			}
		}
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
	// The artwork's alpha is all or nothing (palette index 0); a translucent blend would mix the
	// edges with the world behind, which the original's 8-bit screen never could.
	Tile.BlendMode = Alpha < 1.f ? SE_BLEND_Translucent : SE_BLEND_Masked;
	Canvas->DrawItem(Tile);
	return X + T->GetSizeX() * Scale;
}

float ABRRRaceHUD::TextWidth(const FString& S, float Scale) const
{
	float W = 0;
	for (const TCHAR C : S) W += SpriteWidth(Alphabet, BRR::InterfaceGlyph(C)) * Scale;
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
		Tile.BlendMode = SE_BLEND_Masked;
		Canvas->DrawItem(Tile);
		X += T->GetSizeX() * Scale; // the 8-pixel cell, no gap: as the original sets it
	}
	return X;
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
	// The original draws its SVGA artwork 1:1 on a 640x480 screen; every position below is that
	// screen's, measured on a capture of the original (WATER1, Tools/dosbox-x), times K.
	const float K = H / 480.f;
	// The right-hand items keep their distance from the right edge on a wider screen than the
	// original's 4:3 (the remastered look); on 4:3 this is 0.
	const float Right = FMath::Max(0.f, W - 640.f * K);
	const FBRRRacerStatus Me = Race->Status(Race->PlayerIndex());

	// Top left: the level's map, 1:1, its outline at (2, 2), and every racer's dot on it in its
	// own colour, the player's last.
	if (MapTexture)
	{
		const float Mx = (2.f - MapInset.X) * K, My = (2.f - MapInset.Y) * K;
		FCanvasTileItem Map(FVector2D(Mx, My), MapTexture->GetResource(),
			FVector2D(MapTexture->GetSizeX() * K, MapTexture->GetSizeY() * K), FLinearColor::White);
		Map.BlendMode = SE_BLEND_Masked; // opaque, as the original's 8-bit map
		Canvas->DrawItem(Map);
		auto Dot = [&](int32 R)
		{
			const int32 Glyph = Dot0 + FMath::Clamp(Race->ColourOf(R) - 1, 0, 5);
			const FVector2D Px = TopMap.ToPixel(Race->RacerTransform(R).GetLocation());
			Sprite(Game, Glyph, Mx + Px.X * K - SpriteWidth(Game, Glyph) * K * 0.5f, My + Px.Y * K - SpriteHeight(Game, Glyph) * K * 0.5f, K);
		};
		for (int32 R = 0; R < Race->NumRacers(); ++R)
			if (R != Race->PlayerIndex()) Dot(R);
		Dot(Race->PlayerIndex());
	}

	// Top right: the placing (large digit and ordinal, right edge at 610, top at 24), the boost
	// reserve under it, and the laps completed over the laps in the interface alphabet at twice
	// its size from (550, 108) (its cells end at 598). No clock and no speed: the original shows
	// neither.
	{
		const int32 Place = FMath::Clamp(Me.Place, 1, 9);
		const int32 Ordinal = OrdinalSt + (Place == 1 ? 0 : Place == 2 ? 1 : Place == 3 ? 2 : 3);
		const float PlaceW = (SpriteWidth(Game, Digit0 + Place) + SpriteWidth(Game, Ordinal)) * K;
		Sprite(Game, Ordinal, Sprite(Game, Digit0 + Place, Right + 610.f * K - PlaceW, 24.f * K, K), 24.f * K, K);
		// The boost, in the vehicle's mode (DIRT1's Jeep and WATER1's inflatable, captured): a
		// pip per burst from (546, 84), 18 apart, spent ones dark; or a 76x14 frame at (526, 74)
		// with the red fill over the 72x10 inside from (528, 76), emptying from the right.
		const FBRRBoost Boost = Race->PlayerBoost();
		auto At = [this, K, Right](int32 Glyph, float X, float Y)
		{
			const FIntPoint In = GameInset.IsValidIndex(Glyph) ? GameInset[Glyph] : FIntPoint::ZeroValue;
			Sprite(Game, Glyph, Right + (X - In.X) * K, (Y - In.Y) * K, K);
		};
		if (Boost.Mode == 1)
		{
			for (int32 I = 0; I < Boost.Count; ++I) At(I < Boost.Left ? Pip : PipSpent, 546.f + 18.f * I, 84.f);
		}
		else if (Boost.Mode == 2)
		{
			auto Rect = [this, K, Right](int32 Glyph, FVector2D Uv, float X, float Y, float W, float H)
			{
				if (!Game.IsValidIndex(Glyph) || W <= 0) return;
				FCanvasTileItem Tile(FVector2D(Right + X * K, Y * K), Game[Glyph]->GetResource(), FVector2D(W * K, H * K), Uv, Uv, FLinearColor::White);
				Tile.BlendMode = SE_BLEND_Opaque;
				Canvas->DrawItem(Tile);
			};
			Rect(Bar, FrameTexel, 526.f, 74.f, 76.f, 14.f);
			Rect(Pip, FillTexel, 528.f, 76.f, 72.f * float(FMath::Clamp(Boost.Fraction, 0.0, 1.0)), 10.f);
		}
		const FString Laps = FString::Printf(TEXT("%d/%d"), FMath::Min(Me.Lap, Race->Laps()), Race->Laps());
		Text(Laps, Right + 598.f * K - TextWidth(Laps, 2.f * K), 108.f * K, 2.f * K);
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
