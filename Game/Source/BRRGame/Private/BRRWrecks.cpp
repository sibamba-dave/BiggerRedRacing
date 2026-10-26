#include "BRRWrecks.h"

#include "BRRRace.h"
#include "BRRWorld.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

ABRRWrecks::ABRRWrecks()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

UMaterialInterface* ABRRWrecks::MaterialFor(int32 Atlas, bool bTwoSided)
{
	const TPair<int32, bool> Key(Atlas, bTwoSided);
	if (UMaterialInterface** Found = Materials.Find(Key)) return *Found;
	// The importer's names (BRRImporter MaterialFor).
	const FString Name = FString::Printf(TEXT("MI_%s_%s%s"), *LevelName,
		Atlas == INDEX_NONE ? TEXT("Flat") : *FString::Printf(TEXT("Atlas%02d"), Atlas), bTwoSided ? TEXT("_2S") : TEXT(""));
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("/Game/BRR/%s/Materials/%s.%s"), *LevelName, *Name, *Name));
	if (!Material) UE_LOG(LogTemp, Warning, TEXT("BRRWrecks: %s missing - re-import %s"), *Name, *LevelName);
	Materials.Add(Key, Material);
	MaterialRefs.Add(Material);
	return Material;
}

UProceduralMeshComponent* ABRRWrecks::MakeComponent(const FBRRMesh& Mesh, bool bSolid)
{
	UProceduralMeshComponent* Comp = NewObject<UProceduralMeshComponent>(this);
	Comp->SetupAttachment(GetRootComponent());
	Comp->bUseAsyncCooking = true;
	Comp->bUseComplexAsSimpleCollision = true;
	for (int32 S = 0; S < Mesh.Sections.Num(); ++S)
	{
		const FBRRMeshSection& Section = Mesh.Sections[S];
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> Colors;
		TArray<int32> Triangles;
		const int32 N = Section.Vertices.Num() / 3 * 3;
		Positions.Reserve(N);
		Normals.Reserve(N);
		UVs.Reserve(N);
		Colors.Reserve(N);
		Triangles.Reserve(N);
		for (int32 T = 0; T < N; T += 3)
		{
			// Flat-shaded, as the importer builds it.
			const FVector3f A = Section.Vertices[T].Position;
			const FVector Normal(((Section.Vertices[T + 2].Position - A) ^ (Section.Vertices[T + 1].Position - A)).GetSafeNormal());
			for (int32 K = 0; K < 3; ++K)
			{
				const FBRRVertex& V = Section.Vertices[T + K];
				Triangles.Add(Positions.Add(FVector(V.Position)));
				Normals.Add(Normal);
				UVs.Add(FVector2D(V.UV));
				Colors.Add(V.Color);
			}
		}
		Comp->CreateMeshSection(S, Positions, Triangles, Normals, UVs, Colors, {}, bSolid);
		Comp->SetMaterial(S, MaterialFor(Section.Atlas, Section.bTwoSided));
	}
	if (bSolid)
	{
		Comp->SetCollisionProfileName(TEXT("BlockAll"));
		Comp->SetNotifyRigidBodyCollision(true);
		Comp->OnComponentHit.AddDynamic(this, &ABRRWrecks::OnPieceHit);
	}
	else
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetVisibility(false);
	}
	Comp->SetCastShadow(false);
	Comp->RegisterComponent();
	Components.Add(Comp);
	return Comp;
}

void ABRRWrecks::Build(const TArray<FBRRWreck>& Wrecks, const FString& Level)
{
	LevelName = Level;
	for (const FBRRWreck& W : Wrecks)
	{
		if (W.Intact.NumTriangles() == 0) continue; // a pure billboard; not drawn yet
		FPiece& P = Pieces.AddDefaulted_GetRef();
		P.Instance = W.Instance;
		P.StepMesh = W.StepMesh;
		P.Intact = MakeComponent(W.Intact, true);
		PieceOfComponent.Add(P.Intact, Pieces.Num() - 1);
		for (const FBRRMesh& Step : W.Steps) P.Steps.Add(MakeComponent(Step, false));
	}
}

void ABRRWrecks::OnPieceHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
	// A contact the original's blocker probe did not see (its boxes are not our meshes).
	if (const int32* Piece = PieceOfComponent.Find(HitComponent)) Knocked.AddUnique(Pieces[*Piece].Instance);
}

void ABRRWrecks::Sync(FBRRRace& Race)
{
	for (int32 Instance : Knocked) Race.KnockOver(Instance);
	Knocked.Reset();

	for (FPiece& P : Pieces)
	{
		if (P.bSettled) continue;
		const double Seconds = Race.WreckSeconds(P.Instance);
		if (Seconds < 0) continue;
		if (!P.bDown)
		{
			P.bDown = true;
			P.Intact->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		const int32 Step = FMath::Min(FMath::FloorToInt32(Seconds / BRR::WreckStepSeconds), BRR::WreckStepCount - 1);
		const int32 Want = P.StepMesh.IsValidIndex(Step) ? P.StepMesh[Step] : -1;
		if (Want != P.Shown)
		{
			(P.Shown < 0 ? P.Intact : P.Steps[P.Shown])->SetVisibility(false);
			(Want < 0 ? P.Intact : P.Steps[Want])->SetVisibility(true);
			P.Shown = Want;
		}
		P.bSettled = Step == BRR::WreckStepCount - 1;
	}
}
