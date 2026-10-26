#include "BRRRaceGameMode.h"

#include "BRRArchive.h"
#include "BRRDriveTest.h"
#include "BRRGhostCar.h"
#include "BRRRaceHUD.h"
#include "BRRVehiclePawn.h"
#include "BRRAnimDecor.h"
#include "BRRLook.h"
#include "BRRPhoto.h"
#include "BRRSprites.h"
#include "BRRWrecks.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/PhysicsSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRRace, Log, All);

namespace
{
	constexpr int32 MaxTicksPerFrame = 10;

	TAutoConsoleVariable<int32> CVarViewCells(TEXT("brr.ViewCells"), BRR::ViewCellsDefault,
		TEXT("How far the world is drawn, in terrain cells of view depth: the original's view distance, which it ")
		TEXT("retuned between 10 and 22 against the frame rate. 0 draws the whole map."));

	TAutoConsoleVariable<int32> CVarLook(TEXT("brr.Look"), 1,
		TEXT("0: the classic look, the original's exactly. 1: the remastered look (lit, Lumen, ray traced, upscaled). F5 toggles in a race."));
	TAutoConsoleVariable<int32> CVarPathTracing(TEXT("brr.PathTracing"), 0,
		TEXT("1: the remastered look through the path tracer (for stills: it converges over still frames)."));

	TAutoConsoleVariable<int32> CVarFrameGeneration(TEXT("brr.FrameGeneration"), 1,
		TEXT("1: DLSS Frame Generation in the remastered look, where NVIDIA's Streamline plugin is installed."));

	TAutoConsoleVariable<int32> CVarDlaa(TEXT("brr.DLAA"), 0,
		TEXT("1: the remastered look at full resolution, DLSS anti-aliasing it (DLAA) instead of upscaling from two thirds (TSR: TAA at 100%)."));

	TAutoConsoleVariable<int32> CVarPhotoSamples(TEXT("brr.PhotoSamples"), 512,
		TEXT("Samples per pixel the path tracer gathers for a photo (P in a race)."));

	TAutoConsoleVariable<float> CVarCameraLift(TEXT("brr.CameraLift"), 0.2f,
		TEXT("How much higher the remastered look hangs the chase camera than the original, as a fraction of its distance behind ")
		TEXT("the car, still aimed where the original aims (0: the original's height). The classic look keeps the original's."));

	TAutoConsoleVariable<float> CVarCameraDistance(TEXT("brr.CameraDistance"), 0.6f,
		TEXT("How far behind the car the remastered look hangs the chase camera, as a fraction of the original's distance ")
		TEXT("(1: the original's). The classic look keeps the original's."));

	TAutoConsoleVariable<bool> CVarLogPerf(TEXT("brr.LogPerf"), false,
		TEXT("Logs the frame rate, the race's pace against the wall clock and the player's speed every five seconds."));

	TAutoConsoleVariable<bool> CVarSurfaceHandling(TEXT("brr.SurfaceHandling"), true,
		TEXT("The player's car handles as the surface under it does in the original (off: the track's everywhere)."));

	// The original's 4:3 screen; any other window shape is letterboxed.
	constexpr float ClassicAspect = 4.f / 3.f;
	// The original's vertical field of view, solved from a capture of the original (WATER1 grid,
	// Tools/dosbox-x) against known scenery: a focal length of 476 pixels on its 480-line screen.
	// ORR uses 60 degrees, a choice.
	constexpr double ClassicVerticalFovDegrees = 53.5;
	const float ClassicHorizontalFov = float(FMath::RadiansToDegrees(2.0 * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(ClassicVerticalFovDegrees * 0.5)) * ClassicAspect)));

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

	// A borderless window over the whole desktop (Alt+Enter swaps it for a window). Left alone in
	// the editor, off screen, and when a window is asked for (-windowed, -ResX/-ResY).
	const TCHAR* Cmd = FCommandLine::Get();
	int32 ResX = 0;
	if (!GetWorld()->IsPlayInEditor() && !FParse::Param(Cmd, TEXT("windowed")) && !FParse::Param(Cmd, TEXT("RenderOffScreen"))
		&& !FParse::Value(Cmd, TEXT("ResX="), ResX))
	{
		if (UGameUserSettings* Settings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
		{
			Settings->SetFullscreenMode(EWindowMode::WindowedFullscreen);
			Settings->SetScreenResolution(Settings->GetDesktopResolution());
			Settings->ApplyResolutionSettings(false);
		}
	}

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

bool ABRRRaceGameMode::LoadVehicleMeshes(const FString& Vehicle, int32 Colour, UStaticMesh*& OutBody, TArray<UStaticMesh*>& OutWheels) const
{
	const FString Dir = FString::Printf(TEXT("/Game/BRR/%s/Meshes/"), *LevelName);
	// The body in the racer's team colour (the import's repaint), else the import's own.
	OutBody = LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Body_C%d"), *Vehicle, Colour)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!OutBody) OutBody = LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Body"), *Vehicle)));
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
	Wrecks = GetWorld()->SpawnActor<ABRRWrecks>();
	Wrecks->Build(World.Wrecks, LevelName);
	DrawRange = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/BRR/Materials/MPC_BRR.MPC_BRR"));
	Look = GetWorld()->SpawnActor<ABRRLook>();
	Look->Build(World.SunDirection, DrawRange);
	Photo = GetWorld()->SpawnActor<ABRRPhoto>();
	FParse::Value(FCommandLine::Get(), TEXT("BRRPhoto="), PhotoAt);
	Sprites = GetWorld()->SpawnActor<ABRRSprites>();
	Sprites->Build(World, LevelName);
	AnimDecor = GetWorld()->SpawnActor<ABRRAnimDecor>();
	AnimDecor->Build(World, LevelName);
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
		if (!Data || !LoadVehicleMeshes(Vehicle, Race->ColourOf(R), Body, Wheels))
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
			PlayerCar->SetSurfaces(Handling);
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
	RaceClock += DeltaSeconds;
	if (!Race || !PlayerCar) return;

	// The original's rules and opponents advance in whole 28 ms ticks.
	Accumulator = FMath::Min(Accumulator + DeltaSeconds, MaxTicksPerFrame * FBRRRace::TickSeconds);
	while (Accumulator >= FBRRRace::TickSeconds)
	{
		Accumulator -= FBRRRace::TickSeconds;
		++PerfTicks;
		Race->Tick(PlayerCar->GetBodyOriginTransform(), PlayerCar->GetVelocity(), PlayerCar->IsBoostKeyDown());
		PlayerCar->bBoosting = Race->IsPlayerBoosting();
		PlayerCar->BoostAccel = Race->PlayerBoostAccel();
		PlayerCar->SetSurface(CVarSurfaceHandling.GetValueOnGameThread() ? Race->PlayerSurface() : 0);
		FVector2D Bounce;
		if (Race->ConsumePlayerBounce(Bounce))
		{
			if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(PlayerCar->GetRootComponent()))
			{
				Body->SetPhysicsLinearVelocity(FVector(Bounce.X, Bounce.Y, Body->GetPhysicsLinearVelocity().Z));
			}
		}
		FVector2D Shove;
		double Spin = 0;
		if (Race->ConsumePlayerCarContact(Shove, Spin))
		{
			if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(PlayerCar->GetRootComponent()))
			{
				PlayerCar->AddActorWorldOffset(FVector(Shove, 0), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				FVector Angular = Body->GetPhysicsAngularVelocityInDegrees();
				Angular.Z = Spin;
				Body->SetPhysicsAngularVelocityInDegrees(Angular);
			}
		}
		for (int32 R = 0; R < Race->NumRacers(); ++R)
		{
			PrevPose[R] = CurrPose[R];
			CurrPose[R] = Race->RacerTransform(R);
		}
	}

	if (CVarLogPerf.GetValueOnGameThread())
	{
		const double Now = FPlatformTime::Seconds();
		if (PerfSince < 0) PerfSince = Now, PerfFrames = PerfTicks = 0, PerfPhysics = 0;
		++PerfFrames;
		PerfPhysics += FMath::Min(DeltaSeconds, UPhysicsSettings::Get()->MaxPhysicsDeltaTime);
		if (Now - PerfSince >= 5.0)
		{
			const double Wall = Now - PerfSince;
			UE_LOG(LogBRRRace, Display, TEXT("perf: %.1f fps, race at %.2fx real time, physics at %.2fx, %.0f km/h"), PerfFrames / Wall,
				PerfTicks * FBRRRace::TickSeconds / Wall, PerfPhysics / Wall, PlayerCar->GetVelocity().Size() * 0.036);
			PerfSince = Now;
			PerfFrames = PerfTicks = 0;
			PerfPhysics = 0;
		}
	}
	PlayerCar->bControlsLocked = !Race->HasStarted();
	UpdateStartLights();
	if (Wrecks) Wrecks->Sync(*Race);
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
		const bool bRemastered = CVarLook.GetValueOnGameThread() != 0;
		RaceCamera->SetActorTransform(Race->PlayerCameraTransform(Alpha, bRemastered ? CVarCameraDistance.GetValueOnGameThread() : 1.0,
			bRemastered ? CVarCameraLift.GetValueOnGameThread() : 0.0));
		UE_LOG(LogBRRRace, VeryVerbose, TEXT("camera %s rot %s | car %s rot %s"), *RaceCamera->GetActorLocation().ToCompactString(),
			*RaceCamera->GetActorRotation().ToCompactString(), *PlayerCar->GetBodyOriginTransform().GetLocation().ToCompactString(),
			*PlayerCar->GetActorRotation().ToCompactString());
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			if (PC->WasInputKeyJustPressed(EKeys::C) || PC->WasInputKeyJustPressed(EKeys::Gamepad_RightShoulder)) Race->CyclePlayerCamera();
			if (PC->GetViewTarget() != RaceCamera) PC->SetViewTarget(RaceCamera);
		}
	}
	if (Look)
	{
		if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			if (PC->WasInputKeyJustPressed(EKeys::F5)) CVarLook->Set(Look->IsRemastered() ? 0 : 1, ECVF_SetByConsole);
			if (Photo && (PC->WasInputKeyJustPressed(EKeys::P) || (PhotoAt >= 0 && RaceClock >= PhotoAt)))
			{
				const FString Name = FString::Printf(TEXT("Photo_%s_%s.png"), *LevelName, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
				Photo->Take(Look, CVarPhotoSamples.GetValueOnGameThread(), FPaths::Combine(FPaths::ScreenShotDir(), Name));
				if (PhotoAt >= 0) PhotoAt = -2; // taken: quit once it is saved
			}
			if (PhotoAt == -2 && Photo && !Photo->IsBusy() && !Photo->LastSaved().IsEmpty()) FPlatformMisc::RequestExit(false, TEXT("BRRPhoto"));
		}
		Look->Apply(CVarLook.GetValueOnGameThread() != 0, CVarPathTracing.GetValueOnGameThread() != 0, CVarFrameGeneration.GetValueOnGameThread() != 0,
			CVarDlaa.GetValueOnGameThread() != 0);
		if (RaceCamera)
		{
			// Classic: the original's 4:3, letterboxed. Remastered: the window's own shape, with the
			// original's vertical field of view.
			UCameraComponent* Lens = RaceCamera->GetCameraComponent();
			double Aspect = ClassicAspect;
			if (Look->IsRemastered())
			{
				FVector2D Size(16, 9);
				if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
				Aspect = Size.Y > 0 ? Size.X / Size.Y : 16.0 / 9.0;
			}
			Lens->bConstrainAspectRatio = !Look->IsRemastered();
			Lens->SetFieldOfView(float(FMath::RadiansToDegrees(2.0 * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(ClassicVerticalFovDegrees * 0.5)) * Aspect))));
		}
	}
	if (DrawRange)
	{
		const int32 Cells = Look && Look->IsRemastered() ? 0 : CVarViewCells.GetValueOnGameThread();
		UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), DrawRange, TEXT("DrawDistance"),
			Cells > 0 ? float(Cells * BRR::TerrainCellCm) : 0.f);
	}
	if (Sprites)
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const FRotator Facing = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraRotation()
			: RaceCamera ? RaceCamera->GetActorRotation() : FRotator::ZeroRotator;
		Sprites->Update(*Race, Facing, GetWorld()->GetTimeSeconds());
	}
	if (AnimDecor) AnimDecor->Update(GetWorld()->GetTimeSeconds());
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
	LoadVehicleMeshes(Vehicle, 1, Body, Wheels);
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
	for (int32 Step = 0; Step < 400 && AutopilotCursor < Last; ++Step)
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
