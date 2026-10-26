#include "BRRDriveTest.h"

#include "BRRVehiclePawn.h"
#include "ChaosVehicleWheel.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/PlatformMisc.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRDriveTest, Log, All);

namespace
{
	// A 4 km square slab, far below the level, for straight-line runs.
	const FVector SlabCentre(0, 0, -100000);
	constexpr double SlabScale = 4000.0; // the engine cube is 1 m
}

ABRRDriveTest::ABRRDriveTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
}

void ABRRDriveTest::Start(const FString& DataDir, const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& Wheels)
{
	FString Error;
	if (!BRR::MeasureHandling(DataDir, Vehicle.Name, Reference, Error))
	{
		UE_LOG(LogBRRDriveTest, Error, TEXT("%s"), *Error);
		FPlatformMisc::RequestExit(false, TEXT("BRRDriveTest"));
		return;
	}

	AStaticMeshActor* Slab = GetWorld()->SpawnActor<AStaticMeshActor>(SlabCentre, FRotator::ZeroRotator);
	Slab->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable); // static can't take a mesh at runtime
	Slab->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Slab->SetActorScale3D(FVector(SlabScale, SlabScale, 1.0));

	StartPose = FTransform(FRotator::ZeroRotator, SlabCentre + FVector(-SlabScale * 45.0, 0, 50.0 + 120.0));
	Car = GetWorld()->SpawnActor<ABRRVehiclePawn>(ABRRVehiclePawn::StaticClass(), StartPose);
	Car->Setup(Vehicle, Body, Wheels, Reference.Surfaces[0], StartPose);
	Car->bControlsLocked = false;
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController()) PC->Possess(Car);
	Enter(EPhase::Settle);
}

void ABRRDriveTest::Enter(EPhase Next)
{
	Phase = Next;
	PhaseTime = 0;
	if (Next == EPhase::ReverseSettle || Next == EPhase::TurnSettle) Car->Recover(StartPose);
	switch (Next)
	{
	case EPhase::Accel:
	case EPhase::TurnUp: Car->ScriptedInput = FBRRDriveInput{ true }; break;
	case EPhase::Brake:
	case EPhase::Reverse: Car->ScriptedInput = FBRRDriveInput{ false, true }; break;
	case EPhase::Turn: Car->ScriptedInput = FBRRDriveInput{ true, false, true }; break;
	default: Car->ScriptedInput = FBRRDriveInput{}; break;
	}
}

void ABRRDriveTest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Car || Phase == EPhase::Done) return;
	PhaseTime += DeltaSeconds;
	const double Speed = Car->GetForwardSpeed();
	const double Yaw = Car->GetActorRotation().Yaw;
	if (FMath::FloorToInt(PhaseTime) != FMath::FloorToInt(PhaseTime - DeltaSeconds))
	{
		int32 InContact = 0;
		for (const UChaosVehicleWheel* W : Car->Movement->Wheels) InContact += W && W->IsInAir() ? 0 : 1;
		UE_LOG(LogBRRDriveTest, Verbose, TEXT("phase %d t=%.0f pos %s vel %s fwd %.0f rpm %.0f gear %d->%d thr %.2f brk %.2f hb %.2f wheels %d/%d awake %d ctrl %d"),
			int32(Phase), PhaseTime, *Car->GetActorLocation().ToCompactString(), *Car->GetVelocity().ToCompactString(), Speed,
			Car->Movement->GetEngineRotationSpeed(), Car->Movement->GetCurrentGear(), Car->Movement->GetTargetGear(),
			Car->Movement->GetThrottleInput(), Car->Movement->GetBrakeInput(), float(Car->Movement->GetHandbrakeInput()),
			InContact, Car->Movement->Wheels.Num(), Car->Chassis->IsAnyRigidBodyAwake(), Car->GetController() != nullptr);
	}

	switch (Phase)
	{
	case EPhase::Settle:
		if (PhaseTime > 1.5) Enter(EPhase::Accel);
		break;
	case EPhase::Accel:
		AccelSamples.Add({ PhaseTime, Speed });
		Measured.TopSpeed = FMath::Max(Measured.TopSpeed, Speed);
		if (PhaseTime > 25.0)
		{
			for (const TPair<double, double>& S : AccelSamples)
			{
				if (Measured.TimeTo60 == 0 && S.Value >= 0.6 * Measured.TopSpeed) Measured.TimeTo60 = S.Key;
				if (Measured.TimeTo90 == 0 && S.Value >= 0.9 * Measured.TopSpeed) Measured.TimeTo90 = S.Key;
			}
			Enter(EPhase::Brake);
		}
		break;
	case EPhase::Brake:
		if (Speed <= 0.02 * Measured.TopSpeed || PhaseTime > 15.0)
		{
			Measured.BrakeTime = PhaseTime;
			Enter(EPhase::ReverseSettle);
		}
		break;
	case EPhase::ReverseSettle:
		if (PhaseTime > 1.5) Enter(EPhase::Reverse);
		break;
	case EPhase::Reverse:
		Measured.ReverseTopSpeed = FMath::Max(Measured.ReverseTopSpeed, -Speed);
		if (PhaseTime > 8.0) Enter(EPhase::TurnSettle); // stays on the slab
		break;
	case EPhase::TurnSettle:
		if (PhaseTime > 1.5) Enter(EPhase::TurnUp);
		break;
	case EPhase::TurnUp:
		if (PhaseTime > 4.0)
		{
			PrevYaw = Yaw;
			Enter(EPhase::Turn);
		}
		break;
	case EPhase::Turn:
		if (PhaseTime > 2.0)
		{
			YawAccum += FMath::Abs(FMath::FindDeltaAngleDegrees(PrevYaw, Yaw));
			TurnSpeedAccum += FMath::Abs(Speed) * DeltaSeconds;
			TurnTime += DeltaSeconds;
		}
		PrevYaw = Yaw;
		if (PhaseTime > 4.0)
		{
			Measured.TurnYawRate = TurnTime > 0 ? YawAccum / TurnTime : 0;
			Measured.TurnSpeed = TurnTime > 0 ? TurnSpeedAccum / TurnTime : 0;
			Report();
			Enter(EPhase::Done);
			FPlatformMisc::RequestExit(false, TEXT("BRRDriveTest"));
		}
		break;
	default:
		break;
	}
}

void ABRRDriveTest::Report()
{
	const FBRRSurfaceMetrics& R = Reference.Surfaces[0];
	const FBRRSurfaceMetrics& U = Measured;
	auto Row = [](const TCHAR* Name, double Ref, double Ue, const TCHAR* Unit)
	{
		const double Err = Ref != 0 ? (Ue - Ref) / Ref * 100.0 : 0.0;
		UE_LOG(LogBRRDriveTest, Display, TEXT("  %-16s original %8.2f  chaos %8.2f %-6s %+6.1f%%"), Name, Ref, Ue, Unit, Err);
	};
	UE_LOG(LogBRRDriveTest, Display, TEXT("Drive test %s on TRACK:"), *Reference.Vehicle);
	Row(TEXT("top speed"), R.TopSpeed / 100.0, U.TopSpeed / 100.0, TEXT("m/s"));
	Row(TEXT("0-60% time"), R.TimeTo60, U.TimeTo60, TEXT("s"));
	Row(TEXT("0-90% time"), R.TimeTo90, U.TimeTo90, TEXT("s"));
	Row(TEXT("brake time"), R.BrakeTime, U.BrakeTime, TEXT("s"));
	Row(TEXT("reverse top"), R.ReverseTopSpeed / 100.0, U.ReverseTopSpeed / 100.0, TEXT("m/s"));
	Row(TEXT("turn yaw rate"), R.TurnYawRate, U.TurnYawRate, TEXT("deg/s"));
	Row(TEXT("turn speed"), R.TurnSpeed / 100.0, U.TurnSpeed / 100.0, TEXT("m/s"));
}
