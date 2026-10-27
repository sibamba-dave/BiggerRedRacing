// Photo mode: a path-traced still of the race (P in a race, brr.PhotoSamples).
//
// The path tracer adds one sample per pixel per frame and starts again whenever anything moves,
// so it is no use in motion (UE 5.8's temporal denoiser for it is offline-only). A photo instead
// pauses the race - the camera, the cars, the water's ripples and the clouds hold still - turns the
// remastered look over to the path tracer at full resolution, waits for the samples to gather, has
// the engine's denoiser clean the result, saves it without the HUD, and carries on.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRPhoto.generated.h"

class ABRRLook;

UCLASS()
class BRRGAME_API ABRRPhoto : public AActor
{
	GENERATED_BODY()

public:
	ABRRPhoto();

	// Starts a photo of what the player sees, saved to Path (a .png). False while one is running.
	bool Take(ABRRLook* Look, int32 Samples, const FString& Path);
	bool IsBusy() const { return Step != EStep::Idle; }
	// The photo last saved, empty before the first.
	const FString& LastSaved() const { return Saved; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	enum class EStep : uint8 { Idle, Gathering, Saving };
	EStep Step = EStep::Idle;
	uint64 StepFrame = 0;  // GFrameCounter when the step (re)started
	int32 Samples = 0;
	FString Path;
	FString Saved;
	bool bWasPaused = false;
	bool bHudWasShown = true;

	UPROPERTY()
	TObjectPtr<ABRRLook> Look;

	void Finish();
};
