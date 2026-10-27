// The remastered look's weather and hour, drawn anew for every race, so no two are alike and none is
// promised: the sun low (dawn, dusk) or high, mist some days, and rain or snow as the circuit's
// country has them - more rain by the rivers, snow in the mountains, none in the desert's heat or
// on the moon - at a strength of its own. brr.Weather: 0 always a clear day, 1 drawn (the default),
// 2 always foul.
//
// The rain and snow are a box of streaks and flakes about the camera, built once and animated
// entirely in the shader (M_BRR_Rain / M_BRR_Snow, speeds in MPC_BRR): each frame only carries the
// box along with the camera. Hidden in the classic look (tagged BRRRemasterOnly), which keeps the
// original's sky. brr.Effects 0 hides the fall (as it does the wakes and dust).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRWeather.generated.h"

class ABRRLook;
class UMaterialInterface;
class UProceduralMeshComponent;

UCLASS()
class BRRGAME_API ABRRWeather : public AActor
{
	GENERATED_BODY()

public:
	ABRRWeather();

	// Draws the race's weather for Level and sets the sun and the mist (ABRRLook::SetMood).
	void Build(const FString& Level, ABRRLook* Look);

	// Each frame: carries the box of falling rain or snow along with the camera.
	void Update(const FVector& CameraLocation, bool bOn);

	// What was drawn, for the log and the menus.
	FString Describe() const;

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Fall;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;

	enum class EFall : uint8 { None, Rain, Snow };
	EFall Kind = EFall::None;
	float Strength = 0;        // 0..1
	float SunElevation = 35;   // degrees
	float Mist = 0;            // fog density
	FVector Wind = FVector::ZeroVector; // cm/s
	FRandomStream Random;
	bool bShown = true;
};
