// The remastered look's wakes: what a boat leaves on open water and the original never drew -
// the churned water behind the hull, the two arms of its wake spreading at the Kelvin angle and
// fading, the spray thrown off the bow at speed, and a splash where an arm runs into the bank.
//
// One procedural mesh on /Game/BRR/Materials/M_BRR_Foam, rebuilt each frame from every racer's
// pose, so the ghosts' boats leave wakes as the player's does. Drawn only, and hidden in the
// classic look (tagged BRRRemasterOnly, ABRRLook).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRWake.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
struct FBRRWorld;

UCLASS()
class BRRGAME_API ABRRWake : public AActor
{
	GENERATED_BODY()

public:
	ABRRWake();

	// Where the open water is, at what level, and the remastered ground around it (the banks the
	// wakes run into).
	void Build(const FBRRWorld& World);

	// Each racer's body-origin pose this frame (the race's index order), and whether to draw.
	void Update(const TArray<FTransform>& Poses, float DeltaSeconds, bool bOn);

	// How long a wake's arms last, s.
	static constexpr double Lifetime = 4.0;

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Foam;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;

	// The water: its level per map cell (NaN for none), and the remastered ground's height on a
	// grid Sub per cell across, only near the water (elsewhere +inf).
	int32 Rows = 0, Cols = 0;
	static constexpr int32 Sub = 4;
	TArray<float> CellLevel;     // the lowest water in or beside the cell (NaN for none)
	TArray<float> Surface;       // the drawn water's own height on the Sub grid (NaN off it)
	TArray<float> Ground;
	bool LevelAt(const FVector& P, float& OutLevel) const;
	float GroundAt(const FVector& P) const;

	// A point of a wake: where the boat was on the water, which way it went and how fast, and how
	// far each arm got before it met the bank (the arm's age then; 0 while it runs).
	struct FCrumb
	{
		FVector Position;
		FVector Forward, Right;
		double Speed = 0;
		double Age = 0;
		double Landed[2] = { 0, 0 };
	};
	struct FTrail
	{
		TArray<FCrumb> Crumbs;
		FVector Last = FVector::ZeroVector;
		bool bHaveLast = false;
		double SinceCrumb = 0;
		double Speed = 0;   // cm/s, smoothed
		FVector Heading = FVector::ZeroVector; // the way it is moving, smoothed
		bool bOnWater = false;
	};
	TArray<FTrail> Trails;

	struct FSplash
	{
		FVector Position;
		FVector Across;
		double Age = 0;
		double Size = 1;
	};
	TArray<FSplash> Splashes;
	double Clock = 0;
};
