// The level's destructible decor (FBRRWorld::Wrecks), built when the race starts. A piece stands
// and is solid until the race knocks it over (FBRRRace::WreckSeconds), then plays the original's
// knock-over flipbook with no collision, as a flattened prop no longer resists in the original.
//
// Built at runtime rather than imported: the flipbooks come to over a thousand distinct poses on
// the busiest levels. The importer only leaves a static preview of the standing pieces.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRWrecks.generated.h"

class FBRRRace;
class UMaterialInterface;
class UPrimitiveComponent;
class UProceduralMeshComponent;
struct FBRRMesh;
struct FBRRWreck;

UCLASS()
class BRRGAME_API ABRRWrecks : public AActor
{
	GENERATED_BODY()

public:
	ABRRWrecks();

	// Materials are the import's, /Game/BRR/<Level>/Materials/MI_<Level>_*.
	void Build(const TArray<FBRRWreck>& Wrecks, const FString& Level);

	// Shows each piece as the race has it. Race also receives the knocks Chaos reports.
	void Sync(FBRRRace& Race);

	int32 NumPieces() const { return Pieces.Num(); }

private:
	struct FPiece
	{
		int32 Instance = -1;
		UProceduralMeshComponent* Intact = nullptr;
		TArray<UProceduralMeshComponent*> Steps;
		TArray<int32> StepMesh;
		int32 Shown = -1;      // index into Steps, -1 = the intact piece
		bool bDown = false;
		bool bSettled = false;
	};

	UProceduralMeshComponent* MakeComponent(const FBRRMesh& Mesh, bool bSolid);
	UMaterialInterface* MaterialFor(int32 Atlas, bool bTwoSided);

	UFUNCTION()
	void OnPieceHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	FString LevelName;
	TArray<FPiece> Pieces;
	TMap<UPrimitiveComponent*, int32> PieceOfComponent;
	TArray<int32> Knocked; // instances Chaos hit since the last Sync
	TMap<TPair<int32, bool>, UMaterialInterface*> Materials;
	uint32 SeenRevision = 0;

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> Components;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> MaterialRefs;
};
