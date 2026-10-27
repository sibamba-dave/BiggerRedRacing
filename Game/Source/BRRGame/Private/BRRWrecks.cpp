#include "BRRWrecks.h"

#include "BRRLevelMaterials.h"
#include "BRRRace.h"
#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
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

UProceduralMeshComponent* ABRRWrecks::MakeComponent(const FBRRMesh& Mesh, EPart Part)
{
	UProceduralMeshComponent* Comp = NewObject<UProceduralMeshComponent>(this);
	Comp->SetupAttachment(GetRootComponent());
	Comp->bUseAsyncCooking = true;
	Comp->bUseComplexAsSimpleCollision = true;
	BRR::FillLevelMesh(Comp, Mesh, Part == EPart::Collision, [this](int32 Atlas, bool bTwoSided) { return MaterialFor(Atlas, bTwoSided); });
	if (Part == EPart::Collision)
	{
		// The collision is always the ORIGINAL mesh, whatever look is drawn: both looks collide as
		// the race's geometry, and switching looks never re-cooks a body.
		Comp->SetCollisionProfileName(TEXT("BlockAll"));
		Comp->SetNotifyRigidBodyCollision(true);
		Comp->OnComponentHit.AddDynamic(this, &ABRRWrecks::OnPieceHit);
		Comp->SetVisibility(false);
		Comp->SetCastShadow(false);
	}
	else
	{
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetVisibility(false);
		Comp->SetCastShadow(true); // a standing crate throws a shadow like any solid prop (classic has no shadows at all)
	}
	Comp->RegisterComponent();
	Components.Add(Comp);
	return Comp;
}

void ABRRWrecks::Build(const TArray<FBRRWreck>& Wrecks, const FString& Level)
{
	LevelName = Level;
	static const IConsoleVariable* Look = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.Look"));
	LookSet = Look && Look->GetInt() != 0 ? 1 : 0;
	for (const FBRRWreck& W : Wrecks)
	{
		if (W.Intact.NumTriangles() == 0) continue; // a pure billboard; not drawn yet
		FPiece& P = Pieces.AddDefaulted_GetRef();
		P.Instance = W.Instance;
		P.StepMesh = W.StepMesh;
		P.Collision = MakeComponent(W.Intact, EPart::Collision);
		PieceOfComponent.Add(P.Collision, Pieces.Num() - 1);
		// Both looks' drawn pieces, so F5 switches by visibility alone: classic is the original
		// mesh exactly, remastered the rounded one.
		P.Intact[0] = MakeComponent(W.Intact, EPart::Drawn);
		P.Intact[1] = MakeComponent(BRR::SubdivideSmooth(W.Intact, 2, 50.f), EPart::Drawn);
		for (const FBRRMesh& Step : W.Steps)
		{
			P.Steps[0].Add(MakeComponent(Step, EPart::Drawn));
			P.Steps[1].Add(MakeComponent(BRR::SubdivideSmooth(Step, 2, 50.f), EPart::Drawn));
		}
		Drawn(P)->SetVisibility(true);
	}
}

UProceduralMeshComponent* ABRRWrecks::Drawn(const FPiece& P) const
{
	return P.Shown < 0 ? P.Intact[LookSet] : P.Steps[LookSet][P.Shown];
}

void ABRRWrecks::SetLook(bool bRemastered)
{
	const int32 Want = bRemastered ? 1 : 0;
	if (Want == LookSet) return;
	for (FPiece& P : Pieces)
	{
		Drawn(P)->SetVisibility(false);
		(P.Shown < 0 ? P.Intact[Want] : P.Steps[Want][P.Shown])->SetVisibility(true);
	}
	LookSet = Want;
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
			P.Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		const int32 Step = FMath::Min(FMath::FloorToInt32(Seconds / BRR::WreckStepSeconds), BRR::WreckStepCount - 1);
		const int32 Want = P.StepMesh.IsValidIndex(Step) ? P.StepMesh[Step] : -1;
		if (Want != P.Shown)
		{
			Drawn(P)->SetVisibility(false);
			P.Shown = Want;
			Drawn(P)->SetVisibility(true);
		}
		P.bSettled = Step == BRR::WreckStepCount - 1;
	}
}
