// Interim race HUD (plain text): countdown, lap, place, times, speed, wrong way, missed checkpoint.
// The original's sprite HUD (INGAME.WAP GAME.SPR) replaces it in the classic-look pass.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "BRRRaceHUD.generated.h"

UCLASS()
class BRRGAME_API ABRRRaceHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
