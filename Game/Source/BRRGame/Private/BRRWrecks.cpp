#include "BRRWrecks.h"

#include "BRRLevelMaterials.h"
#include "BRRRace.h"
#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
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
	// The debris' shape and surface, held by the actor so the GC keeps them with the race (and
	// loaded now, not at the first knock, which hitched waiting on the mesh).
	DebrisMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	DebrisPlain = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
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
		// The piece's mean colour, for the chunks it throws when it goes over.
		FLinearColor Sum = FLinearColor::Black;
		int32 N = 0;
		for (const FBRRMeshSection& S : W.Intact.Sections)
			for (const FBRRVertex& V : S.Vertices) { Sum += FLinearColor::FromSRGBColor(FColor(V.Color.R, V.Color.G, V.Color.B)); ++N; }
		if (N > 0) P.Tint = Sum / float(N);
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

void ABRRWrecks::SpawnDebris(const FPiece& P)
{
	// Members, not function statics: a cached raw pointer dangles after a map reload and the
	// second race's first knock-over crashed on it.
	UStaticMesh* Cube = DebrisMesh.Get();
	UMaterialInterface* Plain = DebrisPlain.Get();
	if (!Cube || !Plain || Debris.Num() >= 40) return;
	const FBoxSphereBounds Bounds = P.Collision->Bounds;
	FRandomStream Rnd(P.Instance * 7919 + 11);
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Plain, this);
	Mid->SetVectorParameterValue(TEXT("Color"), P.Tint);
	const int32 Count = Rnd.RandRange(5, 8);
	for (int32 I = 0; I < Count && Debris.Num() < 40; ++I)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(Cube);
		C->SetMaterial(0, Mid);
		C->SetWorldLocation(Bounds.Origin + FVector(Rnd.FRandRange(-0.5f, 0.5f) * Bounds.BoxExtent.X, Rnd.FRandRange(-0.5f, 0.5f) * Bounds.BoxExtent.Y, Bounds.BoxExtent.Z * 0.4));
		C->SetWorldScale3D(FVector(Rnd.FRandRange(0.06f, 0.16f)));
		C->SetWorldRotation(FRotator(Rnd.FRandRange(0.f, 90.f), Rnd.FRandRange(0.f, 360.f), 0));
		// Against the world only: a chunk never nudges a car or the race.
		C->SetCollisionObjectType(ECC_WorldDynamic);
		C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		C->SetCastShadow(false);
		C->RegisterComponent();
		C->SetSimulatePhysics(true);
		C->AddImpulse(FVector(Rnd.FRandRange(-180.f, 180.f), Rnd.FRandRange(-180.f, 180.f), Rnd.FRandRange(220.f, 420.f)), NAME_None, /*bVelChange=*/true);
		C->AddAngularImpulseInDegrees(FVector(Rnd.FRandRange(-360.f, 360.f), Rnd.FRandRange(-360.f, 360.f), Rnd.FRandRange(-360.f, 360.f)), NAME_None, true);
		Debris.Add({ C, GetWorld()->GetTimeSeconds() + Rnd.FRandRange(2.5, 4.0) });
	}
}

void ABRRWrecks::UpdateDebris()
{
	if (Debris.IsEmpty()) return;
	const double Now = GetWorld()->GetTimeSeconds();
	for (int32 I = Debris.Num() - 1; I >= 0; --I)
	{
		UStaticMeshComponent* C = Debris[I].Comp;
		const double Left = Debris[I].Die - Now;
		if (!C || Left <= 0)
		{
			if (C) C->DestroyComponent();
			Debris.RemoveAtSwap(I);
		}
		else if (Left < 0.5) C->SetWorldScale3D(C->GetComponentScale() * 0.94); // sinks away
	}
}

void ABRRWrecks::Sync(FBRRRace& Race)
{
	for (int32 Instance : Knocked) Race.KnockOver(Instance);
	Knocked.Reset();
	UpdateDebris();

	for (FPiece& P : Pieces)
	{
		if (P.bSettled) continue;
		const double Seconds = Race.WreckSeconds(P.Instance);
		if (Seconds < 0) continue;
		if (!P.bDown)
		{
			P.bDown = true;
			P.Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			if (LookSet == 1) SpawnDebris(P); // a cosmetic scatter of chunks, remastered only
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
