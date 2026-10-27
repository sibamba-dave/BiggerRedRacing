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
#include "BRRCardBuffer.h"
#include "BRRLook.h"
#include "GameFramework/Actor.h"

#include "BRRWeather.generated.h"

struct FBRRWorld;
class UMaterialInterface;
class UProceduralMeshComponent;

UCLASS()
class BRRGAME_API ABRRWeather : public AActor
{
	GENERATED_BODY()

public:
	ABRRWeather();

	// Draws the race's weather for Level and sets the sun and the mist (ABRRLook::SetMood). The
	// world supplies the ground heights the rain's splashes land on.
	void Build(const FBRRWorld& World, const FString& Level, ABRRLook* Look);

	// Each frame: carries the box of falling rain or snow along with the camera, and lands the
	// rain - short-lived splash rings on the ground about the camera.
	void Update(const FVector& CameraLocation, float DeltaSeconds, bool bOn);

	// The hour moves on: the sun drifts as the race runs (a long race started at dusk ends in the
	// dark), re-setting the mood every few seconds. Returns true when it crossed the light's
	// dark-race threshold, so the race can switch its lamps.
	bool Advance(float DeltaSeconds, ABRRLook* Look);

	// What was drawn, for the log and the menus.
	FString Describe() const;

	// The drawn hour and rain, for whoever lights by them (the race's head and lamp lights).
	float SunElevationDegrees() const { return SunElevation; }
	float RainStrength() const { return Kind == EFall::Rain ? Strength : 0.f; }

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Fall;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Splashes;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SplashMaterial;

	// Where the rain lands (corner heights, as the dust samples them), and the live rings.
	int32 Rows = 0, Cols = 0;
	TArray<float> CornerZ;
	struct FRing
	{
		FVector At;
		float Age = 0, Life = 0.35f, Seed = 0;
	};
	TArray<FRing> Rings;
	FBRRCardBuffer RingCards;
	float SplashCarry = 0; // fractional spawns carried between frames

	enum class EFall : uint8 { None, Rain, Snow };
	EFall Kind = EFall::None;
	float Strength = 0;        // 0..1
	float SunElevation = 35;   // degrees
	float Mist = 0;            // fog density
	FVector Wind = FVector::ZeroVector; // cm/s
	ABRRLook::FBRRMood Mood;     // as last sent (Advance drifts its sun)
	float SunDriftPerSecond = 0; // degrees; dawn climbs, dusk sinks
	float SinceMood = 0;         // seconds since the mood was last sent
	FRandomStream Random;
};
