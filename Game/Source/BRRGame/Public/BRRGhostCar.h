// An opponent: an original vehicle replayed from its .REC tape. Kinematic (the original never
// pushes a ghost) but solid, so the player's car can bump into it.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRGhostCar.generated.h"

class UBoxComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FBRRVehicleMesh;

UCLASS()
class BRRGAME_API ABRRGhostCar : public AActor
{
	GENERATED_BODY()

public:
	ABRRGhostCar();

	void Setup(const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& WheelMeshes);

	// Moves the body origin to Pose; WheelSpin advances rolling wheels (degrees).
	void SetBodyPose(const FTransform& Pose, double WheelSpinDegrees);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UBoxComponent> Collision;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BodyMesh;

private:
	struct FWheelVisual
	{
		TObjectPtr<UStaticMeshComponent> Mesh;
		bool bRolls = false;
	};
	TArray<FWheelVisual> Wheels;
	FVector BodyOffset = FVector::ZeroVector;
};
