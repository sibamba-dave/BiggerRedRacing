// The player's car: a Chaos wheeled vehicle built from an original vehicle's meshes and fitted to
// the original's measured handling (BRR::MeasureHandling).
//
// Chaos drives the engine, brakes, suspension, gravity and contacts. Steering is the original's own
// model, which no tyre can reproduce (the JEEP corners at ~6 g): the heading turns at the original's
// full-lock yaw rate for the current speed, sideways slip decays as the original's `svel` does, and
// a full-lock turn scrubs speed towards the original's steady turning speed.
#pragma once

#include "CoreMinimal.h"
#include "ChaosVehicleWheel.h"
#include "GameFramework/Pawn.h"

#include "BRRVehiclePawn.generated.h"

class UBoxComponent;
class UCameraComponent;
class UChaosWheeledVehicleMovementComponent;
class USpringArmComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FBRRSurfaceMetrics;
struct FBRRVehicleMesh;

UCLASS()
class UBRRVehicleWheel : public UChaosVehicleWheel
{
	GENERATED_BODY()

public:
	UBRRVehicleWheel();
};

// Digital inputs, as the original reads them.
struct FBRRDriveInput
{
	bool bAccel = false;
	bool bBrake = false;
	bool bLeft = false;
	bool bRight = false;
	bool bHandbrake = false;
};

UCLASS()
class BRRGAME_API ABRRVehiclePawn : public APawn
{
	GENERATED_BODY()

public:
	ABRRVehiclePawn();

	// Builds the car. BodyOrigin is where the original puts the vehicle (its mesh origin).
	void Setup(const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& WheelMeshes,
		const FBRRSurfaceMetrics& Reference, const FTransform& BodyOrigin);

	// The original's notion of the car's position: the body mesh origin.
	FTransform GetBodyOriginTransform() const;
	FVector GetVelocity() const override;
	double GetForwardSpeed() const; // cm/s

	// Puts the car back on its wheels where it is (or at Where), at rest.
	void Recover(const TOptional<FTransform>& Where = {});

	bool bControlsLocked = true;          // until the lights go out
	TOptional<FBRRDriveInput> ScriptedInput; // drive-test / autopilot override

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UBoxComponent> Chassis;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USpringArmComponent> CameraArm;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UChaosWheeledVehicleMovementComponent> Movement;

private:
	struct FWheelVisual
	{
		TObjectPtr<UStaticMeshComponent> Mesh;
		FVector Socket;          // relative to the chassis
		int32 ChaosWheel = INDEX_NONE;
		bool bSteers = false;
	};
	TArray<FWheelVisual> WheelVisuals;
	FVector BodyOffset = FVector::ZeroVector; // body origin relative to the chassis box centre

	TArray<FVector2D> YawCurve;      // the original's full-lock yaw rate (deg/s) by speed (cm/s), under power
	TArray<FVector2D> CoastYawCurve; // ...and coasting
	double TurnScrubSpeed = 0;    // the original's steady full-lock speed, cm/s
	double YawResponse = 4.0;     // 1/s: the original's yaw build-up
	double Steer = 0;             // smoothed steering, -1..1

	static double FullLockYawRate(const TArray<FVector2D>& Curve, double Speed);
	void ApplyOriginalSteering(const FBRRDriveInput& In, float DeltaSeconds);

	FBRRDriveInput ReadPlayerInput() const;
};
