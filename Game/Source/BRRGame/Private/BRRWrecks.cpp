#include "BRRWrecks.h"
#include "BRRArt.h"

#include "BRRLevelMaterials.h"
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
	UMaterialInterface* Material = BRR::LoadLevelMaterial(LevelName, Atlas, bTwoSided);
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
	BRR::FillLevelMesh(Comp, Mesh, bSolid, [this](int32 Atlas, bool bTwoSided) { return MaterialFor(Atlas, bTwoSided); });
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
	Comp->SetCastShadow(true); // a standing crate throws a shadow like any solid prop (classic has no shadows at all)
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
		P.Intact = MakeComponent(BRRArt::LookMesh(W.Intact), true);
		PieceOfComponent.Add(P.Intact, Pieces.Num() - 1);
		for (const FBRRMesh& Step : W.Steps) P.Steps.Add(MakeComponent(BRRArt::LookMesh(Step), false));
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
