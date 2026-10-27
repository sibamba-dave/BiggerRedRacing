// The remastered look's dust haze: what a car on dirt, sand or moon dust leaves hanging in the air
// behind it - big, faint clouds in the ground's own colour that spread, rise a little, drift and
// thin out over a few seconds, so a dirt track under a pack of cars lies in a light fog. The
// original's own puffs (ABRRParticles) still fly off the wheels; this is the air they leave.
//
// One procedural mesh of camera-facing cards on /Game/BRR/Materials/M_BRR_Haze, rebuilt each frame
// from every racer's pose. Drawn only, and hidden in the classic look (tagged BRRRemasterOnly).
#pragma once

#include "CoreMinimal.h"
#include "BRRCardBuffer.h"
#include "GameFramework/Actor.h"

#include "BRRDust.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
struct FBRRWorld;

UCLASS()
class BRRGAME_API ABRRDust : public AActor
{
	GENERATED_BODY()

public:
	ABRRDust();

	// Which ground is dusty, its colour, and its height (a car in the air raises none).
	void Build(const FBRRWorld& World);

	// Each racer's body-origin pose this frame, the camera's right and up, and whether to draw.
	void Update(const TArray<FTransform>& Poses, const FVector& CameraRight, const FVector& CameraUp, float DeltaSeconds, bool bOn);

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Haze;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;

	int32 Rows = 0, Cols = 0;
	TArray<FLinearColor> CellDust; // per map cell: the ground's colour, alpha 0 where it raises no dust
	TArray<float> CornerZ;

	struct FPuff
	{
		FVector Position, Velocity;
		FLinearColor Colour;
		double Age = 0, Life = 5, Size0 = 200, Size1 = 800, Strength = 0.2;
		float Spin = 0;
	};
	TArray<FPuff> Puffs;
	struct FRacer
	{
		FVector Last = FVector::ZeroVector;
		bool bHaveLast = false;
		double Since = 0;
	};
	TArray<FRacer> Racers;
	FRandomStream Random{ 1996 };
	FBRRCardBuffer Cards;
};
