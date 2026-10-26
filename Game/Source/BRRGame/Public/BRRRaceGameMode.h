// Runs a race on an imported level map (/Game/BRR/<LEVEL>/L_<LEVEL>): the player's Chaos car, the
// ghost opponents and the original's rules (FBRRRace) stepped at the original 28 ms tick.
//
// Options: ?Vehicle=JEEP picks the player's car (default: the level's first car).
// Dev: -BRRDriveTest runs the handling comparison against the original on a flat slab and quits;
//      -BRRAutopilot drives the player's car along a ghost's racing line (screenshots, lap tests).
#pragma once

#include "CoreMinimal.h"
#include "BRRRace.h"
#include "BRRWorld.h"
#include "GameFramework/GameModeBase.h"

#include "BRRRaceGameMode.generated.h"

class ACameraActor;
class ABRRGhostCar;
class ABRRVehiclePawn;
class ABRRAnimDecor;
class ABRRLook;
class ABRRSprites;
class UMaterialParameterCollection;
class ABRRWrecks;
class UStaticMesh;
struct FBRRVehicleMesh;

UCLASS()
class BRRGAME_API ABRRRaceGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ABRRRaceGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

	const FBRRRace* GetRace() const { return Race.Get(); }
	const ABRRVehiclePawn* GetPlayerCar() const { return PlayerCar; }
	const FString& GetLevelName() const { return LevelName; }

private:
	bool BuildRace(FString& OutError);
	bool LoadVehicleMeshes(const FString& Vehicle, int32 Colour, UStaticMesh*& OutBody, TArray<UStaticMesh*>& OutWheels) const;
	void RunDriveTest();

	FString LevelName;
	FString DataDir;
	TUniquePtr<FBRRRace> Race;
	TMap<FString, FBRRVehicleMesh> VehicleData;

	UPROPERTY()
	TObjectPtr<ABRRVehiclePawn> PlayerCar;

	UPROPERTY()
	TArray<TObjectPtr<ABRRGhostCar>> Ghosts; // indexed by racer, null for the player

	// The original's chase camera (FBRRRace::PlayerCameraTransform); C cycles its presets.
	UPROPERTY()
	TObjectPtr<ACameraActor> RaceCamera;

	// The start gantry's lamp states from the import (red, red+amber, amber, green); null where missing.
	UPROPERTY()
	TArray<TObjectPtr<AActor>> StartLights;
	int32 ShownStartLight = 0;

	// The destructible decor, built from the level at race start.
	UPROPERTY()
	TObjectPtr<ABRRWrecks> Wrecks;

	// The classic and remastered looks (brr.Look).
	UPROPERTY()
	TObjectPtr<ABRRLook> Look;

	// The import's MPC_BRR: its DrawDistance is the view distance (brr.ViewCells).
	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> DrawRange;

	// The billboards, turned to the camera every frame.
	UPROPERTY()
	TObjectPtr<ABRRSprites> Sprites;

	// The `anim` decor (flags and the like), stepped on the race clock.
	UPROPERTY()
	TObjectPtr<ABRRAnimDecor> AnimDecor;

	void UpdateAutopilot();
	void UpdateStartLights();

	TArray<FTransform> PrevPose, CurrPose;
	TArray<FVector> AutopilotLine;
	int32 AutopilotCursor = 0;
	double AutopilotStuck = 0;
	int32 AutopilotRecoveries = 0;
	int32 LoggedLaps = 0;
	TArray<double> GhostSpin;
	double Accumulator = 0;
	bool bAutopilot = false;
};
