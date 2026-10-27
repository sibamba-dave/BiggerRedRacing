#pragma once

#include "CoreMinimal.h"
#include "BRRCardBuffer.h"
#include "GameFramework/Actor.h"
#include "BRRParticles.generated.h"

struct FBRRMesh;
class UProceduralMeshComponent;

// The race's ground effects (FBRRRace::GetParticles): every live puff of dust, splash and spark,
// one procedural-mesh section per atlas on the import's materials, each built once at capacity and
// refilled each frame (FBRRCardBuffer). In the remastered look each card stands on its point
// instead of straddling it (bStand): half under the single layer water, a splash was seen through
// the water and came out dark.
UCLASS()
class BRRGAME_API ABRRParticles : public AActor
{
	GENERATED_BODY()

public:
	ABRRParticles();
	void Update(const FBRRMesh& Mesh, const FString& Level, bool bStand = false);

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Puffs;
	TMap<int32, FBRRCardBuffer> Buffers;  // by atlas
	TMap<int32, int32> SectionOf;         // atlas -> mesh section
};
