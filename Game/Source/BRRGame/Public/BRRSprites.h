// The level's billboards (FBRRWorld::Sprites): the original's camera-facing quads of a fixed world
// size, rebuilt facing the race camera every frame, one mesh section per atlas. Each shows the
// frame its destructible's flipbook or its `anim` slot calls for, as ORR's sprite_quads picks it.
#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"
#include "GameFramework/Actor.h"

#include "BRRSprites.generated.h"

class FBRRRace;
class UProceduralMeshComponent;

UCLASS()
class BRRGAME_API ABRRSprites : public AActor
{
	GENERATED_BODY()

public:
	ABRRSprites();

	void Build(const FBRRWorld& World, const FString& Level);

	// ElapsedSeconds is the race clock the `anim` slots run on (since the level started).
	void Update(const FBRRRace& Race, const FRotator& CameraRotation, double ElapsedSeconds);

	int32 NumSprites() const { return Sprites.Num(); }

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Quads;

private:
	TArray<FBRRSprite> Sprites;
	TArray<double> AnimStepSeconds;
	TMap<int32, int32> SectionOfAtlas;
};
