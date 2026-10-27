#include "BRRVehiclePawn.h"

#include "BRRRace.h"
#include "BRRWorld.h"
#include "Camera/CameraComponent.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRVehicle, Log, All);

namespace
{
	constexpr float VehicleMassKg = 1500.f;
	constexpr float EngineMaxRPM = 6000.f;
	constexpr float VisualSteerDegrees = 30.f;

	// Fit knobs, tuned with -BRRDriveTest against the original's numbers.
	constexpr float TorqueFit = 1.9f;     // drive test: 0-60% 1.71 s / 0-90% 3.42 s in the original
	constexpr float BrakeFit = 0.83f;     // Chaos stopped in 1.52 s at 1.0 (original 1.82 s)
	constexpr float TyreFriction = 3.5f;
	constexpr double SteerResponse = 8.0; // 1/s, steering input smoothing
	constexpr double SlipDecay = 8.0;     // 1/s, sideways velocity decay on the ground
	constexpr double ScrubRate = 1.0;     // 1/s, speed scrub towards the steady turning speed at full lock
	constexpr double OverSpeedDecay = 2.0; // 1/s, back down to a slower surface's top speed

	// The original's grip switching makes its yaw rate jitter turn to turn; follow its trend.
	TArray<FVector2D> SmoothCurve(const TArray<FVector2D>& In)
	{
		TArray<FVector2D> Out;
		for (int32 I = 0; I < In.Num(); ++I)
		{
			FVector2D Sum = FVector2D::ZeroVector;
			int32 Count = 0;
			for (int32 J = FMath::Max(0, I - 3); J <= FMath::Min(In.Num() - 1, I + 3); ++J, ++Count) Sum += In[J];
			Out.Add(Sum / Count);
		}
		return Out;
	}
}

UBRRVehicleWheel::UBRRVehicleWheel()
{
	WheelRadius = 40.f;
	WheelWidth = 30.f;
	FrictionForceMultiplier = TyreFriction;
	bAffectedByEngine = true;
	bAffectedByBrake = true;
	bAffectedByHandbrake = true;
	bAffectedBySteering = false;
	MaxSteerAngle = 0.f;
	SuspensionMaxRaise = 10.f;
	SuspensionMaxDrop = 15.f;
	// Sideways grip is the original's (ABRRVehiclePawn::ApplyOriginalSteering); stiff tyres would
	// fight the heading it sets.
	CorneringStiffness = 50.f;
}

ABRRVehiclePawn::ABRRVehiclePawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Chassis = CreateDefaultSubobject<UBoxComponent>(TEXT("Chassis"));
	Chassis->SetCollisionProfileName(TEXT("Vehicle"));
	Chassis->SetSimulatePhysics(true);
	Chassis->BodyInstance.bUseCCD = true;
	Chassis->SetNotifyRigidBodyCollision(true); // hit events, for the race's impact sound (NotifyHit)
	SetRootComponent(Chassis);

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	BodyMesh->SetupAttachment(Chassis);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	CameraArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraArm"));
	CameraArm->SetupAttachment(Chassis);
	CameraArm->TargetArmLength = 900.f;
	CameraArm->SocketOffset = FVector(0, 0, 150.f);
	CameraArm->SetRelativeRotation(FRotator(-6.f, 0, 0));
	CameraArm->bEnableCameraLag = true;
	CameraArm->bEnableCameraRotationLag = true;
	CameraArm->CameraRotationLagSpeed = 6.f;
	CameraArm->bInheritPitch = false;
	CameraArm->bInheritRoll = false;
	CameraArm->bDoCollisionTest = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraArm);
	Camera->SetFieldOfView(75.f);

	Movement = CreateDefaultSubobject<UChaosWheeledVehicleMovementComponent>(TEXT("Movement"));
	Movement->SetIsReplicated(false);
	// Chaos disables the drivetrain for good if it ever builds the vehicle without a torque curve,
	// which it does at spawn, before Setup: give it a placeholder that Setup replaces.
	FRichCurve* Torque = Movement->EngineSetup.TorqueCurve.GetRichCurve();
	Torque->AddKey(0.f, 1.f);
	Torque->AddKey(EngineMaxRPM, 1.f);
}

void ABRRVehiclePawn::Setup(const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& WheelMeshes,
	const FBRRSurfaceMetrics& Ref, const FTransform& BodyOrigin)
{
	// The chassis box: the body's footprint, from the wheel centres up, so the tyres reach the ground.
	const FBox3f BodyBox = Vehicle.Body.GetBounds();
	double AxleZ = BodyBox.Min.Z;
	TArray<int32> Rolling;
	for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
	{
		if (!Vehicle.Wheels[I].bRolls) continue;
		Rolling.Add(I);
		AxleZ = FMath::Max(AxleZ, Vehicle.Wheels[I].Socket.Z);
	}
	FBox Box(FVector(BodyBox.Min.X, BodyBox.Min.Y, AxleZ), FVector(BodyBox.Max));
	if (Box.GetSize().Z < 50.0) Box.Max.Z = Box.Min.Z + 50.0;
	BodyOffset = -Box.GetCenter();
	Chassis->SetBoxExtent(Box.GetExtent(), false);
	BodyMesh->SetStaticMesh(Body);
	BodyMesh->SetRelativeLocation(BodyOffset);

	// Wheel visuals, every part (the spare tyre too); Chaos wheels for the rolling ones.
	double WheelRadius = 0, FrontX = -1e9, RearX = 1e9;
	for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
	{
		const FBRRWheel& W = Vehicle.Wheels[I];
		FWheelVisual& Visual = WheelVisuals.AddDefaulted_GetRef();
		Visual.Socket = W.Socket + BodyOffset;
		Visual.bSteers = W.bSteers;
		Visual.Mesh = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("Wheel%d"), I));
		Visual.Mesh->SetStaticMesh(WheelMeshes.IsValidIndex(I) ? WheelMeshes[I] : nullptr);
		Visual.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Visual.Mesh->SetupAttachment(Chassis);
		Visual.Mesh->SetRelativeLocation(Visual.Socket);
		Visual.Mesh->RegisterComponent();
		if (W.bRolls)
		{
			Visual.ChaosWheel = Movement->WheelSetups.Num();
			FChaosWheelSetup& Setup = Movement->WheelSetups.AddDefaulted_GetRef();
			Setup.WheelClass = UBRRVehicleWheel::StaticClass();
			// Chaos insists on a name but only looks it up on a skinned mesh; with this box root the
			// position is AdditionalOffset alone.
			Setup.BoneName = FName(TEXT("Wheel"), I + 1);
			Setup.AdditionalOffset = Visual.Socket;
			WheelRadius += W.Radius;
			if (W.bSteers) FrontX = FMath::Max(FrontX, W.Socket.X);
			else RearX = FMath::Min(RearX, W.Socket.X);
		}
	}
	WheelRadius = Rolling.Num() ? WheelRadius / Rolling.Num() : 40.0;
	const double WheelBaseM = (FrontX > -1e8 && RearX < 1e8) ? (FrontX - RearX) / 100.0 : 2.5;

	// Gearing: one forward gear, so the engine limiter sets the original's top speed.
	const double RadiusM = WheelRadius / 100.0;
	const double TopMps = FMath::Max(Ref.TopSpeed / 100.0, 5.0);
	const double EngineRadPerSec = EngineMaxRPM * 2.0 * PI / 60.0;
	const double Ratio = EngineRadPerSec * RadiusM / TopMps;
	Movement->Mass = VehicleMassKg;
	Movement->DragCoefficient = 0.1f;
	Movement->ChassisWidth = Box.GetSize().Y;
	Movement->ChassisHeight = Box.GetSize().Z;
	Movement->bEnableCenterOfMassOverride = true;
	Movement->CenterOfMassOverride = FVector(0, 0, -Box.GetExtent().Z);
	Movement->bReverseAsBrake = true;

	Movement->TransmissionSetup.bUseAutomaticGears = true;
	Movement->TransmissionSetup.ForwardGearRatios = { 1.f };
	Movement->TransmissionSetup.FinalRatio = float(Ratio);
	const double RevMps = FMath::Max(Ref.ReverseTopSpeed / 100.0, 2.0);
	Movement->TransmissionSetup.ReverseGearRatios = { float(TopMps / RevMps) };
	Movement->TransmissionSetup.ChangeUpRPM = EngineMaxRPM;
	Movement->TransmissionSetup.ChangeDownRPM = 0.f;

	// Torque: the mean acceleration to 60% of top speed, falling off towards the limiter as the
	// original's thrust curve does.
	const double MeanAccel = Ref.TimeTo60 > 0 ? 0.6 * TopMps / Ref.TimeTo60 : 10.0;
	const double WheelForce = VehicleMassKg * MeanAccel * TorqueFit;
	Movement->EngineSetup.MaxRPM = EngineMaxRPM;
	Movement->EngineSetup.EngineIdleRPM = 900.f;
	Movement->EngineSetup.MaxTorque = float(WheelForce * RadiusM / Ratio);
	Movement->EngineSetup.EngineBrakeEffect = 0.05f;
	Movement->EngineSetup.EngineRevUpMOI = 1.f;
	FRichCurve* Torque = Movement->EngineSetup.TorqueCurve.GetRichCurve();
	Torque->Reset();
	Torque->AddKey(0.f, 1.f);
	Torque->AddKey(EngineMaxRPM * 0.6f, 0.6f);
	Torque->AddKey(EngineMaxRPM * 0.9f, 0.37f);
	Torque->AddKey(EngineMaxRPM, 0.27f);

	// Steering is the original's (see ApplyOriginalSteering); Chaos tyres only follow.
	YawCurve = SmoothCurve(Ref.TurnCurve);
	CoastYawCurve = SmoothCurve(Ref.CoastTurnCurve);
	if (Ref.YawTimeConstant > 0) YawResponse = 1.0 / Ref.YawTimeConstant;
	TurnScrubSpeed = Ref.TurnSpeed;

	Movement->DifferentialSetup.DifferentialType = EVehicleDifferential::AllWheelDrive;

	// Rebuild the vehicle with the setup above, then the per-wheel values Chaos reads at runtime.
	Chassis->SetMassOverrideInKg(NAME_None, VehicleMassKg, true);
	Movement->RecreatePhysicsState();
	const double BrakeDecel = Ref.BrakeTime > 0 ? TopMps / Ref.BrakeTime : 10.0;
	const float BrakeTorque = float(VehicleMassKg * BrakeDecel * RadiusM / FMath::Max(Rolling.Num(), 1) * BrakeFit);
	for (int32 I = 0; I < Rolling.Num(); ++I)
	{
		const FBRRWheel& W = Vehicle.Wheels[Rolling[I]];
		Movement->SetWheelRadius(I, float(W.Radius));
		Movement->SetAffectedBySteering(I, false);
		Movement->SetWheelMaxSteerAngle(I, 0.f);
		Movement->SetWheelMaxBrakeTorque(I, BrakeTorque);
		Movement->SetWheelHandbrakeTorque(I, W.bSteers ? 0.f : BrakeTorque * 2.f);
		Movement->SetAffectedByHandbrake(I, !W.bSteers);
	}
	UE_LOG(LogBRRVehicle, Display, TEXT("%s: %d Chaos wheels r=%.0fcm, wheelbase %.2fm, ratio %.2f, torque %.0f Nm, brake %.0f Nm/wheel, %d yaw-curve points"),
		*Vehicle.Name, Rolling.Num(), WheelRadius, WheelBaseM, Ratio, Movement->EngineSetup.MaxTorque, BrakeTorque, YawCurve.Num());

	// Nothing rolls: no Chaos drive to give it. The original's physics moves it (IsSimulated);
	// the box stays for queries only, so nothing pushes it about between the race's ticks.
	bSimulated = Rolling.IsEmpty();
	if (bSimulated)
	{
		Movement->Deactivate();
		Movement->SetComponentTickEnabled(false);
		Chassis->SetSimulatePhysics(false);
		Chassis->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		UE_LOG(LogBRRVehicle, Display, TEXT("%s: on the original's physics"), *Vehicle.Name);
	}

	SetActorTransform(FTransform(BodyOrigin.GetRotation(), BodyOrigin.TransformPosition(-BodyOffset)), false, nullptr, ETeleportType::ResetPhysics);
}

FTransform ABRRVehiclePawn::GetBodyOriginTransform() const
{
	const FTransform Root = GetActorTransform();
	return FTransform(Root.GetRotation(), Root.TransformPosition(BodyOffset));
}

FVector ABRRVehiclePawn::GetVelocity() const
{
	if (bSimulated) return SimVelocity;
	return Chassis ? Chassis->GetPhysicsLinearVelocity() : FVector::ZeroVector;
}

double ABRRVehiclePawn::GetForwardSpeed() const
{
	if (bSimulated) return SimVelocity | GetActorForwardVector();
	return Movement ? Movement->GetForwardSpeed() : 0.0;
}

FBRRDriveInput ABRRVehiclePawn::TakeSimulatedInput(bool& bOutRecover)
{
	bOutRecover = bRecoverKey && !bControlsLocked;
	bRecoverKey = false;
	if (bControlsLocked) return FBRRDriveInput();
	return ScriptedInput.IsSet() ? *ScriptedInput : ReadPlayerInput();
}

void ABRRVehiclePawn::SetSimulatedPose(const FTransform& BodyOrigin, const FVector& Velocity)
{
	SetActorTransform(FTransform(BodyOrigin.GetRotation(), BodyOrigin.TransformPosition(-BodyOffset)), false, nullptr, ETeleportType::TeleportPhysics);
	SimVelocity = Velocity;
	Chassis->ComponentVelocity = Velocity;
}

void ABRRVehiclePawn::Recover(const TOptional<FTransform>& Where)
{
	FTransform Target = Where.Get(GetActorTransform());
	Target.SetRotation(FRotator(0, Target.Rotator().Yaw, 0).Quaternion());
	Target.AddToTranslation(FVector(0, 0, Where.IsSet() ? 0.0 : 150.0));
	SetActorTransform(Target, false, nullptr, ETeleportType::ResetPhysics);
	Chassis->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Chassis->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	bRecovered = true;
}

void ABRRVehiclePawn::SetupCabin(UStaticMesh* Cabin, const TArray<UStaticMesh*>& Parts, const FBRRVehicleMesh& Vehicle, const FString& Level)
{
	if (!Cabin) return;
	auto Attach = [this](UStaticMesh* Mesh, const TCHAR* Name, const FVector& At)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, Name);
		C->SetStaticMesh(Mesh);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false); // the body's shadow stays, cast hidden
		C->SetupAttachment(Chassis);
		C->SetRelativeLocation(At);
		C->SetHiddenInGame(true);
		C->RegisterComponent();
		return C;
	};
	CabinMesh = Attach(Cabin, TEXT("Cabin"), BodyOffset);
	for (int32 I = 0; I < Vehicle.CabinParts.Num() && I < Parts.Num(); ++I)
	{
		const FBRRWheel& P = Vehicle.CabinParts[I];
		CabinPartMeshes.Add(Attach(Parts[I], *FString::Printf(TEXT("CabinPart%d"), I), P.Socket + BodyOffset));
		CabinParts.Add({ P.Socket + BodyOffset, P.Axle, P.bSteers });
	}
	CabinEye = Vehicle.CabinEye;
	CabinPitchDown = Vehicle.CabinPitchDown;

	// The dashboard: its sheet is one of the level's atlas pages; the faces that show it get a
	// live texture instead, redrawn by the race.
	if (Vehicle.DashAtlas != INDEX_NONE)
	{
		DashTexture = UTexture2D::CreateTransient(BRR::AtlasWidth, BRR::AtlasHeight, PF_B8G8R8A8);
		DashTexture->Filter = TF_Nearest;
		DashTexture->SRGB = true;
		DashTexture->UpdateResource();
		const FString Page = FString::Printf(TEXT("_Atlas%02d"), Vehicle.DashAtlas);
		for (int32 M = 0; M < CabinMesh->GetNumMaterials(); ++M)
		{
			UMaterialInterface* Mat = CabinMesh->GetMaterial(M);
			if (!Mat || !Mat->GetName().Contains(Page)) continue;
			UMaterialInstanceDynamic* Live = CabinMesh->CreateDynamicMaterialInstance(M, Mat);
			Live->SetTextureParameterValue(TEXT("Atlas"), DashTexture);
			Live->SetTextureParameterValue(TEXT("AtlasHD"), DashTexture);
		}
	}
}

void ABRRVehiclePawn::SetCabinView(bool bOn)
{
	if (!CabinMesh || bOn == bInCabin) return;
	bInCabin = bOn;
	// The car's own body would fill the lens: hidden, its shadow kept.
	BodyMesh->SetHiddenInGame(bOn);
	BodyMesh->bCastHiddenShadow = true;
	for (const FWheelVisual& W : WheelVisuals)
	{
		W.Mesh->SetHiddenInGame(bOn);
		W.Mesh->bCastHiddenShadow = true;
	}
	CabinMesh->SetHiddenInGame(!bOn);
	for (UStaticMeshComponent* P : CabinPartMeshes) P->SetHiddenInGame(!bOn);
}

FTransform ABRRVehiclePawn::GetCabinEyeTransform() const
{
	const FTransform Body = GetBodyOriginTransform();
	const FRotator R = Body.Rotator();
	const FRotator Eye(R.Pitch * 7.0 / 8.0 - CabinPitchDown, R.Yaw, R.Roll * 5.0 / 6.0);
	return FTransform(Eye, Body.TransformPosition(CabinEye));
}

void ABRRVehiclePawn::UpdateDashboard(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	if (!DashTexture || Pixels.Num() != Width * Height || Width != DashTexture->GetSizeX() || Height != DashTexture->GetSizeY()) return;
	// The render thread reads the copy after this returns, and frees it.
	FColor* Copy = new FColor[Pixels.Num()];
	FMemory::Memcpy(Copy, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
	DashTexture->UpdateTextureRegions(0, 1, Region, Width * sizeof(FColor), sizeof(FColor), reinterpret_cast<uint8*>(Copy),
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			delete[] reinterpret_cast<FColor*>(Data);
			delete Regions;
		});
}

void ABRRVehiclePawn::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	// The ground is not an obstacle: landings are the race's to hear (VehicleSim::landing_impact).
	const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(OtherComp);
	if (!Mesh || !Mesh->GetStaticMesh() || Mesh->GetStaticMesh()->GetName().EndsWith(TEXT("_Terrain"))) return;
	const double Mass = Chassis ? Chassis->GetMass() : 0.0;
	const double Speed = Mass > 0 ? NormalImpulse.Size() / Mass : 0.0;
	if (Speed > ImpactSpeed)
	{
		ImpactSpeed = Speed;
		ImpactPoint = HitLocation;
	}
}

void ABRRVehiclePawn::SetSurfaces(const FBRRHandlingMetrics& Handling)
{
	// Each surface against the track the engine and brakes were fitted to (Setup): its share of
	// the track's acceleration and braking, its top speed, its own turning.
	const FBRRSurfaceMetrics& Track = Handling.Surfaces[0];
	auto Accel = [](const FBRRSurfaceMetrics& M) { return M.TimeTo60 > 0 ? 0.6 * M.TopSpeed / M.TimeTo60 : 0.0; };
	auto Decel = [](const FBRRSurfaceMetrics& M) { return M.BrakeTime > 0 ? M.TopSpeed / M.BrakeTime : 0.0; };
	Surfaces.Reset();
	for (const FBRRSurfaceMetrics& M : Handling.Surfaces)
	{
		FSurfaceHandling& S = Surfaces.AddDefaulted_GetRef();
		S.Yaw = SmoothCurve(M.TurnCurve);
		S.CoastYaw = SmoothCurve(M.CoastTurnCurve);
		S.YawResponse = M.YawTimeConstant > 0 ? 1.0 / M.YawTimeConstant : YawResponse;
		S.ScrubSpeed = M.TurnSpeed;
		S.TopSpeed = M.TopSpeed;
		S.Throttle = Accel(Track) > 0 ? FMath::Clamp(Accel(M) / Accel(Track), 0.0, 1.0) : 1.0;
		S.Brake = Decel(Track) > 0 ? FMath::Clamp(Decel(M) / Decel(Track), 0.0, 1.0) : 1.0;
	}
	CurrentSurface = -1;
	SetSurface(0);
}

void ABRRVehiclePawn::SetSurface(int32 Surface)
{
	if (Surface == CurrentSurface || !Surfaces.IsValidIndex(Surface)) return;
	CurrentSurface = Surface;
	const FSurfaceHandling& S = Surfaces[Surface];
	if (!S.Yaw.IsEmpty()) YawCurve = S.Yaw;
	if (!S.CoastYaw.IsEmpty()) CoastYawCurve = S.CoastYaw;
	YawResponse = S.YawResponse;
	TurnScrubSpeed = S.ScrubSpeed;
	SurfaceTopSpeed = S.TopSpeed;
	ThrottleScale = S.Throttle;
	BrakeScale = S.Brake;
}

FBRRDriveInput ABRRVehiclePawn::ReadPlayerInput() const
{
	FBRRDriveInput In;
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return In;
	auto Down = [PC](const FKey& Key) { return PC->IsInputKeyDown(Key); };
	In.bAccel = Down(EKeys::Up) || Down(EKeys::W) || PC->GetInputAnalogKeyState(EKeys::Gamepad_RightTriggerAxis) > 0.3f;
	In.bBrake = Down(EKeys::Down) || Down(EKeys::S) || PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftTriggerAxis) > 0.3f;
	const float StickX = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	In.bLeft = Down(EKeys::Left) || Down(EKeys::A) || StickX < -0.35f;
	In.bRight = Down(EKeys::Right) || Down(EKeys::D) || StickX > 0.35f;
	In.bHandbrake = Down(EKeys::SpaceBar) || Down(EKeys::Gamepad_FaceButton_Left);
	In.bBoost = Down(EKeys::LeftShift) || Down(EKeys::RightShift) || Down(EKeys::Gamepad_FaceButton_Bottom);
	return In;
}

void ABRRVehiclePawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bSimulated)
	{
		// The race reads the controls (TakeSimulatedInput); only the recover key's press is kept.
		if (const APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			bRecoverKey |= PC->WasInputKeyJustPressed(EKeys::R) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top);
		}
		bBoostKey = !bControlsLocked && (ScriptedInput.IsSet() ? ScriptedInput->bBoost : ReadPlayerInput().bBoost);
		return;
	}

	FBRRDriveInput In = ScriptedInput.IsSet() ? *ScriptedInput : ReadPlayerInput();
	if (bControlsLocked)
	{
		// The original holds every car where the grid put it until the lights go out. Chaos left
		// alone lets a boat on the water slide and turn into the barrier, so here the car may only
		// settle on its suspension.
		In = FBRRDriveInput{ false, false, false, false, true };
		if (Chassis->IsAnyRigidBodyAwake())
		{
			Chassis->SetPhysicsLinearVelocity(FVector(0, 0, Chassis->GetPhysicsLinearVelocity().Z));
			FVector Angular = Chassis->GetPhysicsAngularVelocityInDegrees();
			Angular.Z = 0;
			Chassis->SetPhysicsAngularVelocityInDegrees(Angular);
		}
	}
	bBoostKey = In.bBoost;
	if (bBoosting)
	{
		In.bAccel = true;
		In.bBrake = false;
		Chassis->AddForce(GetActorForwardVector() * BoostAccel, NAME_None, /*bAccelChange=*/true);
	}
	// The surface's top speed: past it the throttle is off and the car is brought back down.
	const double Fwd = GetForwardSpeed();
	const bool bOverSpeed = SurfaceTopSpeed > 0 && Fwd > SurfaceTopSpeed;
	if (bOverSpeed) Chassis->AddForce(-GetActorForwardVector() * (Fwd - SurfaceTopSpeed) * OverSpeedDecay, NAME_None, /*bAccelChange=*/true);
	Movement->SetThrottleInput(In.bAccel && !bOverSpeed ? float(ThrottleScale) : 0.f);
	Movement->SetBrakeInput(In.bBrake ? float(BrakeScale) : 0.f);
	Movement->SetSteeringInput((In.bRight ? 1.f : 0.f) - (In.bLeft ? 1.f : 0.f));
	Movement->SetHandbrakeInput(In.bHandbrake);
	// A car that settled on the grid goes to sleep, and Chaos does not wake it for the drivetrain.
	if ((In.bAccel || In.bBrake || In.bLeft || In.bRight) && !Chassis->IsAnyRigidBodyAwake()) Chassis->WakeAllRigidBodies();

	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (PC->WasInputKeyJustPressed(EKeys::R) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top)) Recover();
	}

	ApplyOriginalSteering(In, DeltaSeconds);

	// Wheel visuals: steer about Z, roll about the axle.
	for (const FWheelVisual& Visual : WheelVisuals)
	{
		if (Visual.ChaosWheel == INDEX_NONE || !Movement->Wheels.IsValidIndex(Visual.ChaosWheel)) continue;
		const UChaosVehicleWheel* Wheel = Movement->Wheels[Visual.ChaosWheel];
		const FQuat SteerRot(FVector::UpVector, Visual.bSteers ? FMath::DegreesToRadians(Steer * VisualSteerDegrees) : 0.0);
		const FQuat Roll(FVector::RightVector, -FMath::DegreesToRadians(Wheel->GetRotationAngle()));
		Visual.Mesh->SetRelativeRotation(SteerRot * Roll);
		Visual.Mesh->SetRelativeLocation(Visual.Socket + FVector(0, 0, Wheel->GetSuspensionOffset()));
	}
	// The cabin's steering wheel turns with the steering, about its own axis.
	for (int32 I = 0; bInCabin && I < CabinParts.Num() && I < CabinPartMeshes.Num(); ++I)
	{
		if (!CabinParts[I].bSteers) continue;
		CabinPartMeshes[I]->SetRelativeRotation(FQuat(CabinParts[I].Axle, FMath::DegreesToRadians(Steer * 90.0)));
	}
}

double ABRRVehiclePawn::FullLockYawRate(const TArray<FVector2D>& Curve, double Speed)
{
	if (Curve.IsEmpty()) return 0.0;
	if (Speed <= Curve[0].X) return Curve[0].Y * (Curve[0].X > 0 ? Speed / Curve[0].X : 0.0);
	for (int32 I = 1; I < Curve.Num(); ++I)
	{
		if (Speed <= Curve[I].X)
		{
			const double T = (Speed - Curve[I - 1].X) / FMath::Max(Curve[I].X - Curve[I - 1].X, 1.0);
			return FMath::Lerp(Curve[I - 1].Y, Curve[I].Y, T);
		}
	}
	return Curve.Last().Y;
}

void ABRRVehiclePawn::ApplyOriginalSteering(const FBRRDriveInput& In, float DeltaSeconds)
{
	const double Target = (In.bRight ? 1.0 : 0.0) - (In.bLeft ? 1.0 : 0.0);
	Steer = FMath::FInterpTo(Steer, Target, DeltaSeconds, SteerResponse);

	int32 OnGround = 0;
	for (const UChaosVehicleWheel* W : Movement->Wheels) OnGround += W && !W->IsInAir() ? 1 : 0;
	if (OnGround == 0 || DeltaSeconds <= 0.f) return; // in the air the original keeps its heading too

	const FVector Forward = GetActorForwardVector();
	const FVector Right = GetActorRightVector();
	FVector Velocity = Chassis->GetPhysicsLinearVelocity();
	const double ForwardSpeed = Velocity | Forward;

	// Heading: the original's yaw rate for this speed, reversed when rolling backwards.
	FVector Angular = Chassis->GetPhysicsAngularVelocityInDegrees();
	const double WantYaw = Steer * FullLockYawRate(In.bAccel ? YawCurve : CoastYawCurve, FMath::Abs(ForwardSpeed)) * FMath::Sign(ForwardSpeed);
	Angular.Z = FMath::FInterpTo(Angular.Z, WantYaw, DeltaSeconds, YawResponse);
	Chassis->SetPhysicsAngularVelocityInDegrees(Angular);

	// Grip: sideways slip dies away; a full-lock turn bleeds speed towards the steady turning speed.
	const double Lateral = Velocity | Right;
	Velocity -= Right * Lateral * (1.0 - FMath::Exp(-SlipDecay * DeltaSeconds));
	const double Over = FMath::Abs(ForwardSpeed) - TurnScrubSpeed;
	if (Over > 0 && TurnScrubSpeed > 0)
	{
		Velocity -= Forward * FMath::Sign(ForwardSpeed) * Over * FMath::Abs(Steer) * (1.0 - FMath::Exp(-ScrubRate * DeltaSeconds));
	}
	Chassis->SetPhysicsLinearVelocity(Velocity);
}
