// The remastered look's trackside life: country folk come to watch - in overalls and plaid, straw
// hats and trucker caps, some sat in lawn chairs by the cooler - in knots beside the start and the
// checkpoints, off the track and out of the water, facing the racing and jumping up as the cars come
// by; and the flags on their poles, flying in the wind (BRR::FindFlags; the props give the poles).
// Its people are the import's (SM_BRR_Fan<n>_Shirt / _Rest, BRR::MakeSpectator), each shirt tinted.
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
	// Each frame: the racers' places (for the cheering), the wind, the clock.
	void Update(const TArray<FTransform>& Racers, double Seconds, bool bOn);
	int32 NumPeople() const { return People.Num(); }

private:
	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Parts; // variant * 2 + (0 shirt, 1 rest)
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Cloth;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> ClothMaterial;

	struct FPerson
	{
		FTransform Stand;
		int32 Variant = 0;
		int32 Instance = 0;   // in its variant's two components
		float Phase = 0, Pace = 1;
		float Excitement = 0;
	};
	TArray<FPerson> People;
	TArray<BRR::FBRRFlag> Flags;
	bool bShown = true;
};
