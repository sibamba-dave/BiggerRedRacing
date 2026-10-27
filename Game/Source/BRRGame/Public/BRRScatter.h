// The remastered look's ground detail: grass tufts on grass, pebbles on earth and sand, rocks on
// rock, within reach of the track (BRR::ScatterGround), each coloured as the ground under it.
// Drawn only, never solid, and hidden in the classic look (tagged BRRRemasterOnly, ABRRLook).
//
// Placed when the race starts rather than imported: a few hundred thousand instances would make
// each map tens of megabytes, and they come from the level data in a fraction of a second. The
// shapes are the import's (/Game/BRR/Meshes/SM_BRR_Grass<n>, SM_BRR_Stone<n>).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRScatter.generated.h"

class UInstancedStaticMeshComponent;
struct FBRRWorld;

UCLASS()
class BRRGAME_API ABRRScatter : public AActor
{
	GENERATED_BODY()

public:
	ABRRScatter();
	void Build(const FBRRWorld& World);
	int32 NumInstances() const;

private:
	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Shapes;
};
