// The remastered look's trackside life: country folk come to watch - in overalls and plaid, straw
// hats and trucker caps, some sat in lawn chairs by the cooler - in knots beside the start and the
// checkpoints, off the track and out of the water, facing the racing and jumping up as the cars come
// by; and the flags on their poles, flying in the wind (BRR::FindFlags; the props give the poles).
// Its people are the import's (SM_BRR_Fan<n>_Shirt / _Rest, BRR::MakeSpectator), each shirt tinted.
// Everything animates in the shader (M_BRR_Crowd, M_BRR_Cloth): the build writes each person's
// phase and pace as instance data, the race writes the racers' positions into MPC_BRR once a frame,
// and after that no instance or vertex is touched again.
// brr.Crowds 0 leaves the stands empty. Hidden in the classic look (tagged BRRRemasterOnly).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BRRProps.h"

#include "BRRCrowd.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UProceduralMeshComponent;
struct FBRRWorld;

UCLASS()
class BRRGAME_API ABRRCrowd : public AActor
{
	GENERATED_BODY()

public:
	ABRRCrowd();
	// Lines: where the cars go (the ghosts' routes): nobody stands within a lane's reach of them.
	void Build(const FBRRWorld& World, const TArray<FVector>& Lines);
	// Each frame: only shows or hides with the look; the cheering runs in the shader.
	void Update(bool bOn);
	int32 NumPeople() const { return PeopleCount; }

private:
	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Parts; // variant * 2 + (0 shirt, 1 rest)
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Cloth;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> CrowdMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> ClothMaterial;

	int32 PeopleCount = 0;
	bool bShown = true;
};
