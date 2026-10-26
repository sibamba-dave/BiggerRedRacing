#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BRRParticles.generated.h"

struct FBRRMesh;
class UProceduralMeshComponent;

// The race's ground effects (FBRRRace::GetParticles): every live puff of dust, splash and spark,
// rebuilt each frame as one procedural mesh on the import's atlas materials.
UCLASS()
class BRRGAME_API ABRRParticles : public AActor
{
	GENERATED_BODY()

public:
	ABRRParticles();
	void Update(const FBRRMesh& Mesh, const FString& Level);

private:
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Puffs;
	UPROPERTY()
	TMap<int32, TObjectPtr<UMaterialInterface>> Materials;
	int32 Sections = 0;
};
