#include "BRRGhostCar.h"

#include "BRRWorld.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"

ABRRGhostCar::ABRRGhostCar()
{
	Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	Collision->SetCollisionProfileName(TEXT("BlockAll"));
	// Not solid to the player's car: a replay is teleported tick to tick, and Chaos shoves out
	// whatever it lands in. The race runs the original's car-to-car contact instead
	// (FBRRRace::ConsumePlayerCarContact).
	Collision->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
	Collision->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Collision);

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	BodyMesh->SetupAttachment(Collision);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void ABRRGhostCar::Setup(const FBRRVehicleMesh& Vehicle, UStaticMesh* Body, const TArray<UStaticMesh*>& WheelMeshes)
{
	FBox Box(Vehicle.Body.GetBounds());
	for (const FBRRWheel& W : Vehicle.Wheels)
	{
		const FBox3f WB = W.Mesh.GetBounds();
		Box += FVector(WB.Min) + W.Socket;
		Box += FVector(WB.Max) + W.Socket;
	}
	BodyOffset = -Box.GetCenter();
	Collision->SetBoxExtent(Box.GetExtent());
	BodyMesh->SetStaticMesh(Body);
	BodyMesh->SetRelativeLocation(BodyOffset);
	for (int32 I = 0; I < Vehicle.Wheels.Num(); ++I)
	{
		FWheelVisual& Visual = Wheels.AddDefaulted_GetRef();
		Visual.bRolls = Vehicle.Wheels[I].bRolls;
		Visual.Mesh = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("Wheel%d"), I));
		Visual.Mesh->SetStaticMesh(WheelMeshes.IsValidIndex(I) ? WheelMeshes[I] : nullptr);
		Visual.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Visual.Mesh->SetupAttachment(Collision);
		Visual.Mesh->SetRelativeLocation(Vehicle.Wheels[I].Socket + BodyOffset);
		Visual.Mesh->RegisterComponent();
	}
}

void ABRRGhostCar::SetBodyPose(const FTransform& Pose, double WheelSpinDegrees)
{
	SetActorTransform(FTransform(Pose.GetRotation(), Pose.TransformPosition(-BodyOffset)));
	const FRotator Roll(-WheelSpinDegrees, 0, 0);
	for (const FWheelVisual& W : Wheels)
	{
		if (W.bRolls) W.Mesh->SetRelativeRotation(Roll);
	}
}
