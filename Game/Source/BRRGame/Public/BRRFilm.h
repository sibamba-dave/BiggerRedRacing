// The remastered intro's race shots, filmed (Tools/brr_film.py): `-BRRFilm=<plan.json>
// -BRRFilmOut=<dir>` on a circuit's map films every shot the plan gives that circuit, one after
// another in one race, and quits. The race runs at a fixed 30 frames a second however long a
// frame takes to draw; each shot holds its camera on its racer (chase, side, trackside, high or
// front), settles for a few frames, then saves as many frames as the shot lasts in the original,
// <dir>/shot_<id>/<frame>.png. The HUD is off; the look is the one asked for (brr.Look).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRFilm.generated.h"

class ACameraActor;

UCLASS()
class BRRGAME_API ABRRFilm : public AActor
{
	GENERATED_BODY()

public:
	ABRRFilm();

	// Reads the plan's shots for Level. False when there are none.
	bool Load(const FString& PlanPath, const FString& OutDir, const FString& Level);

	// Each frame, after the race's own camera: the racers' poses as drawn and the race clock (s
	// since the start; negative before it).
	void Update(const TArray<FTransform>& Racers, double RaceSeconds, ACameraActor* Camera);

	static constexpr double FrameSeconds = 1.0 / 30.0;

private:
	struct FShot
	{
		int32 Id = 0;
		int32 Frames = 0;      // to save, at 30 a second
		FString Camera;
		int32 Racer = 0;
	};
	TArray<FShot> Shots;
	FString OutDir;
	int32 Current = 0;
	int32 Frame = -13;         // within the current shot; negative while it settles (from -SettleFrames - 1, a cut)
	FVector Anchor = FVector::ZeroVector; // a trackside camera's place
	FVector Eye = FVector::ZeroVector, Look = FVector::ZeroVector; // smoothed
	double StartAt = 2.0;      // s after the lights: the pack has spread a little
	bool bDone = false;
};
