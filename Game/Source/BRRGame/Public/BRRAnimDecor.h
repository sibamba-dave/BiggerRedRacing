// The level's animated decor (FBRRWorld::AnimSteps): each `anim` slot's poses, one shown at a time
// on the race clock, every instance on a slot in phase, as the original's shared accumulator keeps
// them. Only what a pose adds is here; what all poses share is in the static decor.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRAnimDecor.generated.h"

class UProceduralMeshComponent;
struct FBRRWorld;

UCLASS()
class BRRGAME_API ABRRAnimDecor : public AActor
{
	GENERATED_BODY()

public:
	ABRRAnimDecor();

	void Build(const FBRRWorld& World, const FString& Level);

	// ElapsedSeconds is the race clock (since the level started), as for the billboards.
	void Update(double ElapsedSeconds);

	int32 NumSlots() const { return Slots.Num(); }

private:
	struct FSlot
	{
		int64 StepMs = 0;
		TArray<UProceduralMeshComponent*> Steps; // null where a step adds nothing
		int32 Shown = -1;
	};
	TArray<FSlot> Slots;

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> Components;
};
