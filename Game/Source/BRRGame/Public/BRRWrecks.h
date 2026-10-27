// The level's destructible decor (FBRRWorld::Wrecks), built when the race starts. A piece stands
// and is solid until the race knocks it over (FBRRRace::WreckSeconds), then plays the original's
// knock-over flipbook with no collision, as a flattened prop no longer resists in the original.
//
// Built at runtime rather than imported: the flipbooks come to over a thousand distinct poses on
// the busiest levels. The importer only leaves a static preview of the standing pieces.
//
// Collision is one invisible component per piece from the ORIGINAL mesh, whatever look is drawn:
// both looks collide as the race's geometry. The drawn pieces exist in both looks (the original,
// and the rounded remaster), and F5 switches them by visibility alone (SetLook, no hitch).
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

	// F5: which look's drawn pieces show (ABRRLook::OnLookChanged).
	void SetLook(bool bRemastered);

	int32 NumPieces() const { return Pieces.Num(); }

private:
	struct FPiece
	{
		int32 Instance = -1;
		UProceduralMeshComponent* Collision = nullptr; // the original mesh, invisible, both looks
		UProceduralMeshComponent* Intact[2] = {};      // drawn: [0] classic, [1] remastered
		TArray<UProceduralMeshComponent*> Steps[2];
		TArray<int32> StepMesh;
		FLinearColor Tint = FLinearColor::White; // the piece's mean colour, for its debris
		int32 Shown = -1;      // index into Steps, -1 = the intact piece
		bool bDown = false;
		bool bSettled = false;
	};
	// Cosmetic chunks thrown as a piece goes over (remastered only): simulated against the world
	// alone - never a car - and gone in a few seconds. The original's flipbook stays the truth.
	struct FDebris
	{
		class UStaticMeshComponent* Comp = nullptr;
		double Die = 0;
	};
	TArray<FDebris> Debris;
	void SpawnDebris(const FPiece& P);
	void UpdateDebris();
	enum class EPart : uint8 { Collision, Drawn };

	UProceduralMeshComponent* MakeComponent(const FBRRMesh& Mesh, EPart Part);
	UProceduralMeshComponent* Drawn(const FPiece& P) const;
	UMaterialInterface* MaterialFor(int32 Atlas, bool bTwoSided);
	int32 LookSet = 1;

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
