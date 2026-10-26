// The original's race rules and opponents, driven from UE.
//
// FBRRRace runs ORR's RaceState (gates, CHECKP lines, lap validity, placing, wrong way, countdown)
// and GhostDriver (the .REC opponents with the original's rubber band) at the original's 28 ms
// physics tick. The player's car is simulated by UE (Chaos); each tick its pose is handed in and
// mirrored into a proxy of the original vehicle so the rules read it exactly as they read a car.
//
// BRR::MeasureHandling runs the original vehicle simulation on a synthetic flat map, once per
// surface type, to obtain the reference numbers UE's vehicle is tuned against.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

// One surface type's reference handling (all speeds cm/s, times s).
struct FBRRSurfaceMetrics
{
	double TopSpeed = 0;         // full throttle, straight
	double TimeTo60 = 0;         // from rest to 60% / 90% of TopSpeed
	double TimeTo90 = 0;
	double BrakeTime = 0;        // TopSpeed to a stop, brake held
	double ReverseTopSpeed = 0;  // brake held from rest
	double TurnSpeed = 0;        // speed held through the turn test (full throttle + full lock)
	double TurnYawRate = 0;      // deg/s during it
	// Full-lock yaw rate (Y, deg/s) against speed (X, cm/s), ascending speed, sampled along held
	// full-lock turns: under full throttle from rest (the original steers far harder under power)
	// and coasting down from top speed.
	TArray<FVector2D> TurnCurve;
	TArray<FVector2D> CoastTurnCurve;
	double YawTimeConstant = 0;  // s for the yaw rate to reach 63% of its steady value at full lock
};

struct FBRRHandlingMetrics
{
	FString Vehicle;
	FBRRSurfaceMetrics Surfaces[8]; // TRACK GRIPPY SLIPPY CLIFFS GRASS SAND RAMPS AIR

	static const TCHAR* SurfaceName(int32 Surface);
};

namespace BRR
{
	BRRFORMATS_API bool MeasureHandling(const FString& DataDir, const FString& Vehicle, FBRRHandlingMetrics& Out, FString& OutError);
}

struct FBRRRacerStatus
{
	int32 Lap = 0;             // laps completed
	int32 Place = 1;           // 1 = leading
	bool bFinished = false;
	bool bWrongWay = false;
	bool bMissedCheckpoint = false;
	bool bCheckpointThisTick = false;
	bool bLapThisTick = false;
	TArray<double> LapTimes;   // seconds, one per completed lap
};

class BRRFORMATS_API FBRRRace
{
public:
	static constexpr double TickSeconds = 0.028;

	// Builds the six-car field as the single race does: grid block b is driven by b.REC in its
	// recorded vehicle and colour, except the player's block (the level's single-race start place).
	static TUniquePtr<FBRRRace> Create(const FString& DataDir, const FString& Level, const FString& PlayerVehicle, FString& OutError);
	~FBRRRace();

	int32 NumRacers() const;
	int32 PlayerIndex() const;
	bool IsGhost(int32 Racer) const;
	FString VehicleOf(int32 Racer) const;
	int32 ColourOf(int32 Racer) const;   // team colour 1..6

	// Body-origin transform (UE) of a racer as of the last tick (or its grid pose before the first).
	FTransform RacerTransform(int32 Racer) const;

	// The whole route a ghost's tape drives at normal speed (UE body origins, one per tick): a
	// racing line for autopilots and tests. Empty for a racer without a tape.
	TArray<FVector> GhostPath(int32 Racer) const;

	// Advances one 28 ms tick. PlayerPose is the player's body-origin transform, PlayerVelocity cm/s.
	void Tick(const FTransform& PlayerPose, const FVector& PlayerVelocity);

	// The original's chase camera on the player, stepped with each tick; Alpha (0..1) blends the
	// last two ticks. Returns the eye and its orientation in UE space.
	FTransform PlayerCameraTransform(double Alpha) const;
	void CyclePlayerCamera();          // the three distance presets

	// Destructible decor (FBRRWorld::Wrecks, by scenery instance), shared by the whole field. The
	// player knocks a piece over when the original's blocker probe touches it, as in
	// VehicleSim::hit_scenery; ghosts never do (a replay has no scenery pass).
	// Seconds since Instance went down, negative while it stands. A piece that went down while
	// the original's 20 flipbook slots were busy reads as long settled.
	double WreckSeconds(int32 Instance) const;
	// Knocks Instance over now - a UE contact the probe missed. Ignored if it is already down.
	void KnockOver(int32 Instance);
	uint32 WreckRevision() const;      // changes whenever anything is knocked over
	// The player's new ground velocity (UE X/Y, cm/s) after knocking a piece over: the original's
	// push-back. True once per bounce; apply it to the car and keep its vertical speed.
	bool ConsumePlayerBounce(FVector2D& OutVelocityXY);

	bool HasStarted() const;           // the lights are out
	int32 CountdownNumber() const;     // 3, 2, 1 while counting, 0 at "go", -1 otherwise
	bool CountdownBeeped() const;      // a countdown beep fell in the last tick
	bool IsRaceOver() const;
	int32 Laps() const;
	double RaceTime() const;           // seconds since the start
	FBRRRacerStatus Status(int32 Racer) const;

private:
	FBRRRace();
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
