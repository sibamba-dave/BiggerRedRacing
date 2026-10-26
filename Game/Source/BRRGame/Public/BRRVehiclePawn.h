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
struct FBRRHandlingMetrics;
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
	bool bBoost = false;
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

	// The original's handling differs by surface (FBRRHandlingMetrics::SurfaceName). Given all
	// eight, the car follows the one under it (SetSurface, each tick): its turning, its share of
	// the track's acceleration and braking, and its top speed.
	void SetSurfaces(const FBRRHandlingMetrics& Handling);
	void SetSurface(int32 Surface);

	// The original's notion of the car's position: the body mesh origin.
	FTransform GetBodyOriginTransform() const;
	FVector GetVelocity() const override;
	double GetForwardSpeed() const; // cm/s

	// Puts the car back on its wheels where it is (or at Where), at rest.
	void Recover(const TOptional<FTransform>& Where = {});

	// The cabin view (FBRRRace::IsPlayerInCabin): the vehicle's interior and its parts shown in
	// place of the body, the eye at the vehicle's `internal` offset, and the dashboard's gauges
	// redrawn onto its sheet (FBRRRace::DrawDashboard) through UpdateDashboard.
	void SetupCabin(UStaticMesh* Cabin, const TArray<UStaticMesh*>& Parts, const FBRRVehicleMesh& Vehicle, const FString& Level);
	bool HasCabin() const { return CabinMesh != nullptr; }
	void SetCabinView(bool bOn);
	// The original's cabin eye: turned with the car, its pitch and roll following the body's only
	// in part (7/8 and 5/6), looking down by the vehicle's own angle (ORR cabin_view).
	FTransform GetCabinEyeTransform() const;
	void UpdateDashboard(const TArray<FColor>& Pixels, int32 Width, int32 Height);

	// For the race's sound, read and cleared by the game mode: the hardest knock against solid
	// scenery since (speed into it, cm/s), and whether the car was put back on the track.
	double TakeImpactSpeed() { const double S = ImpactSpeed; ImpactSpeed = 0; return S; }
	FVector ImpactPoint = FVector::ZeroVector; // where the hardest of them struck
	bool TakeRecovered() { const bool b = bRecovered; bRecovered = false; return b; }
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
		FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

	bool bControlsLocked = true;          // until the lights go out
	TOptional<FBRRDriveInput> ScriptedInput; // drive-test / autopilot override

	// The boost key as last read (false while the controls are locked); the race runs the boost.
	bool IsBoostKeyDown() const { return bBoostKey; }
	// Set by the race each tick: while a boost burns, the accelerator is held down, the brake
	// ignored, and BoostAccel (cm/s/s) pushes the car along its heading - the original's rules.
	bool bBoosting = false;
	double BoostAccel = 0;

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

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> CabinMesh;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> CabinPartMeshes;

	UPROPERTY()
	TObjectPtr<class UTexture2D> DashTexture;

private:
	bool bBoostKey = false;
	double ImpactSpeed = 0;
	bool bRecovered = false;
	bool bInCabin = false;
	FVector CabinEye = FVector::ZeroVector;
	double CabinPitchDown = 0;
	struct FCabinPart { FVector Socket, Axle; bool bSteers = false; };
	TArray<FCabinPart> CabinParts;
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

	struct FSurfaceHandling
	{
		TArray<FVector2D> Yaw, CoastYaw;
		double YawResponse = 4.0, ScrubSpeed = 0, TopSpeed = 0, Throttle = 1, Brake = 1;
	};
	TArray<FSurfaceHandling> Surfaces;
	int32 CurrentSurface = -1;
	double SurfaceTopSpeed = 0;   // cm/s, 0 = the engine's own
	double ThrottleScale = 1, BrakeScale = 1;
	double Steer = 0;             // smoothed steering, -1..1

	static double FullLockYawRate(const TArray<FVector2D>& Curve, double Speed);
	void ApplyOriginalSteering(const FBRRDriveInput& In, float DeltaSeconds);

	FBRRDriveInput ReadPlayerInput() const;
};
