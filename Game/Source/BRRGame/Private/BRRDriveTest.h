// -BRRDriveTest[=VEHICLE]: drives the Chaos car through the same tests BRR::MeasureHandling runs
// on the original simulation (on a flat TRACK slab), logs both side by side and quits.
#pragma once

#include "CoreMinimal.h"
#include "BRRRace.h"
#include "BRRWorld.h"
#include "GameFramework/Actor.h"

#include "BRRDriveTest.generated.h"

class ABRRVehiclePawn;
class UStaticMesh;

UCLASS()
class ABRRDriveTest : public AActor
{
	GENERATED_BODY()

public:
	ABRRDriveTest();
	void Start(const FString& DataDir, const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& Wheels);
	virtual void Tick(float DeltaSeconds) override;

private:
	enum class EPhase { Settle, Accel, Brake, ReverseSettle, Reverse, TurnSettle, TurnUp, Turn, Done };
	void Enter(EPhase Next);
	void Report();

	UPROPERTY()
	TObjectPtr<ABRRVehiclePawn> Car;

	FBRRHandlingMetrics Reference;
	FBRRSurfaceMetrics Measured;
	FTransform StartPose;
	EPhase Phase = EPhase::Done;
	double PhaseTime = 0;
	TArray<TPair<double, double>> AccelSamples; // time, speed
	double YawAccum = 0, TurnSpeedAccum = 0, TurnTime = 0, PrevYaw = 0;
};
