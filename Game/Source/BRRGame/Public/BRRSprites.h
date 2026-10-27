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

	// ElapsedSeconds is the race clock the `anim` slots run on (since the level started). RangeCm
	// culls sprites further than that from the camera (0 draws them all); the quads are rebuilt
	// only when a frame, the camera's facing or (by more than a lane) its place has changed.
	void Update(const FBRRRace& Race, const FVector& CameraLocation, const FRotator& CameraRotation, double ElapsedSeconds, float RangeCm);

	int32 NumSprites() const { return Sprites.Num(); }

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Quads;

private:
	TArray<FBRRSprite> Sprites;
	TArray<double> AnimStepSeconds;
	TMap<int32, int32> SectionOfAtlas;
	TArray<int32> LastKs;          // each sprite's frame as last built
	FRotator LastRotation = FRotator::ZeroRotator;
	FVector LastLocation = FVector::ZeroVector;
	float LastRange = -1.f;        // -1: nothing built yet
};
