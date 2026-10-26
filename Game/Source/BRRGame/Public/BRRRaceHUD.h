// The race board, drawn with the original's own artwork: GAMESVGA.SPR (INGAME.WAP) digits,
// ordinals and banners, the SML_ALPH.SPR interface alphabet and the level's TOPMAP.SPR mini-map,
// all through their palettes and magnified with nearest filtering. Layout follows ORR's race board.
#pragma once

#include "CoreMinimal.h"
#include "BRRAssets.h"
#include "GameFramework/HUD.h"

#include "BRRRaceHUD.generated.h"

class UTexture2D;

UCLASS()
class BRRGAME_API ABRRRaceHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	bool LoadArtwork();
	static UTexture2D* MakeTexture(const FBRRImage& Image);

	// Draw helpers, in screen pixels; each returns the pen's new x.
	float Sprite(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index, float X, float Y, float Scale, float Alpha = 1.f);
	float Text(const FString& S, float X, float Y, float Scale, const FLinearColor& Tint = FLinearColor::White);
	float TextWidth(const FString& S, float Scale) const;
	float Number(int32 Value, float X, float Y, float Scale);
	float NumberWidth(int32 Value, float Scale) const;
	float Clock(double Seconds, float X, float Y, float Scale);
	float ClockWidth(float Scale) const;
	void Centred(int32 Index, float CX, float Y, float Scale);
	float SpriteWidth(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index) const;
	float SpriteHeight(const TArray<TObjectPtr<UTexture2D>>& Sheet, int32 Index) const;

	bool bTriedLoad = false;

	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> Game;     // GAMESVGA.SPR

	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> Alphabet; // SML_ALPH.SPR

	UPROPERTY()
	TObjectPtr<UTexture2D> MapTexture;

	FBRRTopMap TopMap;
	int32 FinalLapShownAt = -1; // race time (s) the final lap began, for the banner
	double FinalLapStart = 0;
};
