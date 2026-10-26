#include "BRRRaceGameMode.h"

#include "BRRArchive.h"
#include "BRRDriveTest.h"
#include "BRRGhostCar.h"
#include "BRRRaceHUD.h"
#include "BRRVehiclePawn.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRRace, Log, All);

namespace
{
	constexpr int32 MaxTicksPerFrame = 10;

	// The original's 4:3 screen and 60-degree vertical field; any other window shape is letterboxed.
	constexpr float ClassicAspect = 4.f / 3.f;
	const float ClassicHorizontalFov = float(FMath::RadiansToDegrees(2.0 * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(30.0)) * ClassicAspect)));

	// "L_DIRT1" (or a PIE "UEDPIE_0_L_DIRT1") -> "DIRT1"; empty for any other map.
	FString LevelFromMap(const UWorld* World)
	{
		const FString Map = UWorld::RemovePIEPrefix(World->GetMapName());
		return Map.StartsWith(TEXT("L_")) ? Map.RightChop(2) : FString();
	}
}

ABRRRaceGameMode::ABRRRaceGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	HUDClass = ABRRRaceHUD::StaticClass();
}

void ABRRRaceGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// On a race map the player's car is built in StartPlay; anywhere else, the usual default pawn.
	if (LevelFromMap(GetWorld()).IsEmpty())
	{
		Super::HandleStartingNewPlayer_Implementation(NewPlayer);
	}
	else if (PlayerCar)
	{
		NewPlayer->Possess(PlayerCar);
		if (RaceCamera) NewPlayer->SetViewTarget(RaceCamera);
	}
}

void ABRRRaceGameMode::StartPlay()
{
	Super::StartPlay();
	LevelName = LevelFromMap(GetWorld());
	if (LevelName.IsEmpty()) return;

	if (FParse::Param(FCommandLine::Get(), TEXT("BRRDriveTest")))
	{
		RunDriveTest();
		return;
	}

	FString Error;
	if (!BuildRace(Error))
	{
		UE_LOG(LogBRRRace, Error, TEXT("Cannot race %s: %s"), *LevelName, *Error);
		return;
	}
	bAutopilot = FParse::Param(FCommandLine::Get(), TEXT("BRRAutopilot"));
	for (int32 R = 0; bAutopilot && R < Race->NumRacers() && AutopilotLine.IsEmpty(); ++R)
	{
		if (Race->IsGhost(R)) AutopilotLine = Race->GhostPath(R);
	}
	RaceCamera = GetWorld()->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Race->PlayerCameraTransform(0.0));
	UCameraComponent* Lens = RaceCamera->GetCameraComponent();
	Lens->bConstrainAspectRatio = true;
	Lens->SetAspectRatio(ClassicAspect);
	Lens->SetFieldOfView(ClassicHorizontalFov);
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->Possess(PlayerCar);
		PC->SetViewTarget(RaceCamera);
	}
}

bool ABRRRaceGameMode::LoadVehicleMeshes(const FString& Vehicle, UStaticMesh*& OutBody, TArray<UStaticMesh*>& OutWheels) const
{
	const FString Dir = FString::Printf(TEXT("/Game/BRR/%s/Meshes/"), *LevelName);
	OutBody = LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Body"), *Vehicle)));
	OutWheels.Reset();
	const FBRRVehicleMesh* Data = VehicleData.Find(Vehicle);
	for (int32 I = 0; Data && I < Data->Wheels.Num(); ++I)
	{
		OutWheels.Add(LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Wheel%d"), *Vehicle, I))));
	}
	return OutBody != nullptr;
}

bool ABRRRaceGameMode::BuildRace(FString& OutError)
{
	DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		OutError = TEXT("game data not found (set BRR_DATA_DIR)");
		return false;
	}

	// The level's cars: wheel sockets and sizes come from the original data, meshes from the import.
	FBRRWorld Probe;
	if (!FBRRWorld::Load(DataDir, LevelName, {}, 1, Probe, OutError)) return false;
	FBRRWorld World;
	if (!FBRRWorld::Load(DataDir, LevelName, Probe.CarList, 1, World, OutError)) return false;
	for (FBRRVehicleMesh& V : World.Vehicles) VehicleData.Add(V.Name, MoveTemp(V));

	FString PlayerVehicle = UGameplayStatics::ParseOption(OptionsString, TEXT("Vehicle")).ToUpper();
	if (!VehicleData.Contains(PlayerVehicle)) PlayerVehicle = Probe.CarList.IsEmpty() ? TEXT("JEEP") : Probe.CarList[0];

	Race = FBRRRace::Create(DataDir, LevelName, PlayerVehicle, OutError);
	if (!Race) return false;

	FBRRHandlingMetrics Handling;
	if (!BRR::MeasureHandling(DataDir, PlayerVehicle, Handling, OutError)) return false;

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TEXT("BRRPreview"))) It->Destroy();
	}
	StartLights.Init(nullptr, 4);
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		for (int32 K = 0; K < StartLights.Num(); ++K)
		{
			if (It->ActorHasTag(*FString::Printf(TEXT("BRRStartLight%d"), K))) StartLights[K] = *It;
		}
	}
	ShownStartLight = -1;
	UpdateStartLights();

	const int32 N = Race->NumRacers();
	Ghosts.SetNum(N);
	GhostSpin.SetNumZeroed(N);
	for (int32 R = 0; R < N; ++R)
	{
		const FString Vehicle = Race->VehicleOf(R);
		const FBRRVehicleMesh* Data = VehicleData.Find(Vehicle);
		UStaticMesh* Body = nullptr;
		TArray<UStaticMesh*> Wheels;
		if (!Data || !LoadVehicleMeshes(Vehicle, Body, Wheels))
		{
			OutError = FString::Printf(TEXT("%s meshes missing: import %s first (Tools > Big Red Racing)"), *Vehicle, *LevelName);
			return false;
		}
		const FTransform Start = Race->RacerTransform(R);
		PrevPose.Add(Start);
		CurrPose.Add(Start);
		if (R == Race->PlayerIndex())
		{
			PlayerCar = GetWorld()->SpawnActor<ABRRVehiclePawn>(ABRRVehiclePawn::StaticClass(), Start);
			PlayerCar->Setup(*Data, Body, Wheels, Handling.Surfaces[0], Start);
		}
		else if (Race->IsGhost(R))
		{
			ABRRGhostCar* Ghost = GetWorld()->SpawnActor<ABRRGhostCar>(ABRRGhostCar::StaticClass(), Start);
			Ghost->Setup(*Data, Body, Wheels);
			Ghost->SetBodyPose(Start, 0);
			Ghosts[R] = Ghost;
		}
	}
	UE_LOG(LogBRRRace, Display, TEXT("%s: %d racers, player %s on block %d, %d laps"), *LevelName, N, *PlayerVehicle,
		Race->PlayerIndex(), Race->Laps());
	return PlayerCar != nullptr;
}

void ABRRRaceGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Race || !PlayerCar) return;

	// The original's rules and opponents advance in whole 28 ms ticks.
	Accumulator = FMath::Min(Accumulator + DeltaSeconds, MaxTicksPerFrame * FBRRRace::TickSeconds);
	while (Accumulator >= FBRRRace::TickSeconds)
	{
		Accumulator -= FBRRRace::TickSeconds;
		Race->Tick(PlayerCar->GetBodyOriginTransform(), PlayerCar->GetVelocity());
		for (int32 R = 0; R < Race->NumRacers(); ++R)
		{
			PrevPose[R] = CurrPose[R];
			CurrPose[R] = Race->RacerTransform(R);
		}
	}

	PlayerCar->bControlsLocked = !Race->HasStarted();
	UpdateStartLights();
	if (bAutopilot && Race->HasStarted()) UpdateAutopilot();

	const FBRRRacerStatus Me = Race->Status(Race->PlayerIndex());
	if (Me.LapTimes.Num() > LoggedLaps)
	{
		LoggedLaps = Me.LapTimes.Num();
		UE_LOG(LogBRRRace, Display, TEXT("Lap %d/%d in %.2f s, place %d%s"), LoggedLaps, Race->Laps(), Me.LapTimes.Last(), Me.Place,
			Me.bFinished ? TEXT(" - FINISHED") : TEXT(""));
	}

	// Ghosts and the camera drawn between their last two ticks.
	const double Alpha = Accumulator / FBRRRace::TickSeconds;
	if (RaceCamera)
	{
		RaceCamera->SetActorTransform(Race->PlayerCameraTransform(Alpha));
		UE_LOG(LogBRRRace, VeryVerbose, TEXT("camera %s rot %s | car %s rot %s"), *RaceCamera->GetActorLocation().ToCompactString(),
			*RaceCamera->GetActorRotation().ToCompactString(), *PlayerCar->GetBodyOriginTransform().GetLocation().ToCompactString(),
			*PlayerCar->GetActorRotation().ToCompactString());
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			if (PC->WasInputKeyJustPressed(EKeys::C) || PC->WasInputKeyJustPressed(EKeys::Gamepad_RightShoulder)) Race->CyclePlayerCamera();
			if (PC->GetViewTarget() != RaceCamera) PC->SetViewTarget(RaceCamera);
		}
	}
	for (int32 R = 0; R < Ghosts.Num(); ++R)
	{
		if (!Ghosts[R]) continue;
		const FVector Location = FMath::Lerp(PrevPose[R].GetLocation(), CurrPose[R].GetLocation(), Alpha);
		const FQuat Rotation = FQuat::Slerp(PrevPose[R].GetRotation(), CurrPose[R].GetRotation(), Alpha);
		const FVector Moved = Location - Ghosts[R]->GetActorLocation();
		const double Forward = Moved | Rotation.GetForwardVector();
		GhostSpin[R] = FMath::Fmod(GhostSpin[R] + FMath::RadiansToDegrees(Forward / 40.0), 360.0);
		Ghosts[R]->SetBodyPose(FTransform(Rotation, Location), GhostSpin[R]);
	}
}

void ABRRRaceGameMode::RunDriveTest()
{
	DataDir = BRR::GetDataDir();
	FString Error;
	FBRRWorld Probe;
	if (DataDir.IsEmpty() || !FBRRWorld::Load(DataDir, LevelName, {}, 1, Probe, Error))
	{
		UE_LOG(LogBRRRace, Error, TEXT("Drive test: %s"), *Error);
		return;
	}
	const FString Vehicle = FParse::Value(FCommandLine::Get(), TEXT("BRRDriveTest="), Error) ? Error.ToUpper()
		: (Probe.CarList.IsEmpty() ? FString(TEXT("JEEP")) : Probe.CarList[0]);
	FBRRWorld World;
	if (!FBRRWorld::Load(DataDir, LevelName, { Vehicle }, 1, World, Error) || World.Vehicles.IsEmpty())
	{
		UE_LOG(LogBRRRace, Error, TEXT("Drive test: %s"), *Error);
		return;
	}
	VehicleData.Add(Vehicle, World.Vehicles[0]);
	UStaticMesh* Body = nullptr;
	TArray<UStaticMesh*> Wheels;
	LoadVehicleMeshes(Vehicle, Body, Wheels);
	ABRRDriveTest* Test = GetWorld()->SpawnActor<ABRRDriveTest>();
	Test->Start(DataDir, World.Vehicles[0], Body, Wheels);
}

void ABRRRaceGameMode::UpdateStartLights()
{
	// The gantry is the countdown - the original prints no number: red, red+amber, amber on the
	// three beeps, then green for the rest of the race (as ORR's race screen picks it).
	const int32 N = Race->CountdownNumber();
	const int32 State = N >= 3 ? 0 : N == 2 ? 1 : N == 1 ? 2 : 3;
	if (State == ShownStartLight) return;
	ShownStartLight = State;
	for (int32 K = 0; K < StartLights.Num(); ++K)
	{
		AStaticMeshActor* Lights = Cast<AStaticMeshActor>(StartLights[K]);
		if (Lights) Lights->GetStaticMeshComponent()->SetVisibility(K == State);
	}
}

void ABRRRaceGameMode::UpdateAutopilot()
{
	if (AutopilotLine.IsEmpty())
	{
		PlayerCar->ScriptedInput = FBRRDriveInput{ true };
		return;
	}
	// Follow the ghost's line: advance the cursor past points already reached, aim 25 m on.
	const FVector Here = PlayerCar->GetBodyOriginTransform().GetLocation();
	auto Dist2D = [&Here](const FVector& P) { return FVector::Dist2D(Here, P); };
	const int32 Last = AutopilotLine.Num() - 1;
	for (int32 Look = 0; Look < 400 && AutopilotCursor < Last; ++Look)
	{
		// Jump forward to the nearest point within the next stretch of line.
		int32 Best = AutopilotCursor;
		for (int32 I = AutopilotCursor; I <= FMath::Min(AutopilotCursor + 400, Last); ++I)
		{
			if (Dist2D(AutopilotLine[I]) < Dist2D(AutopilotLine[Best])) Best = I;
		}
		if (Best == AutopilotCursor) break;
		AutopilotCursor = Best;
	}
	int32 Target = AutopilotCursor;
	while (Target < Last && Dist2D(AutopilotLine[Target]) < 1500.0) ++Target;

	// Stuck against something: put the car back on the line, as the original's recover key does.
	AutopilotStuck = FMath::Abs(PlayerCar->GetForwardSpeed()) < 300.0 ? AutopilotStuck + GetWorld()->GetDeltaSeconds() : 0.0;
	if (AutopilotStuck > 2.0)
	{
		const int32 Ahead = FMath::Min(AutopilotCursor + 40, Last);
		const FVector Dir = (AutopilotLine[Ahead] - AutopilotLine[AutopilotCursor]).GetSafeNormal2D();
		PlayerCar->Recover(FTransform(Dir.Rotation(), AutopilotLine[AutopilotCursor] + FVector(0, 0, 150.0)));
		AutopilotStuck = 0;
		++AutopilotRecoveries;
		UE_LOG(LogBRRRace, Display, TEXT("Autopilot: recovered onto the line (%d) at point %d/%d, was at %s"), AutopilotRecoveries,
			AutopilotCursor, Last, *Here.ToCompactString());
	}

	const FVector To = (AutopilotLine[Target] - Here).GetSafeNormal2D();
	const FVector Forward = PlayerCar->GetActorForwardVector().GetSafeNormal2D();
	const double Angle = FMath::RadiansToDegrees(FMath::Atan2(Forward.X * To.Y - Forward.Y * To.X, Forward | To));
	// Digital steering, as a player has: steer on the heading error the car will have once its
	// current yaw rate has played out (the original's yaw builds and decays over ~0.6 s).
	const double YawRate = PlayerCar->GetRootComponent()->GetComponentVelocity().IsZero() ? 0.0
		: Cast<UPrimitiveComponent>(PlayerCar->GetRootComponent())->GetPhysicsAngularVelocityInDegrees().Z;
	const double Predicted = Angle - YawRate * 0.45;
	const double Speed = PlayerCar->GetForwardSpeed();
	FBRRDriveInput In;
	In.bRight = Predicted > 3.0;
	In.bLeft = Predicted < -3.0;
	In.bBrake = FMath::Abs(Angle) > 35.0 && Speed > 2500.0;
	In.bAccel = !In.bBrake;
	PlayerCar->ScriptedInput = In;
}
