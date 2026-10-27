#include "BRRAnimDecor.h"
#include "BRRArt.h"

#include "BRRLevelMaterials.h"
#include "BRRWorld.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

ABRRAnimDecor::ABRRAnimDecor()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void ABRRAnimDecor::Build(const FBRRWorld& World, const FString& Level)
{
	TMap<TPair<int32, bool>, UMaterialInterface*> Materials;
	auto MaterialFor = [&](int32 Atlas, bool bTwoSided)
	{
		UMaterialInterface*& M = Materials.FindOrAdd({ Atlas, bTwoSided });
		if (!M) M = BRR::LoadLevelMaterial(Level, Atlas, bTwoSided);
		return M;
	};
	for (int32 S = 0; S < World.AnimSteps.Num(); ++S)
	{
		FSlot& Slot = Slots.AddDefaulted_GetRef();
		Slot.StepMs = World.AnimStepSeconds.IsValidIndex(S) ? FMath::RoundToInt64(World.AnimStepSeconds[S] * 1000.0) : 0;
		for (const FBRRMesh& Step : World.AnimSteps[S])
		{
			UProceduralMeshComponent* Comp = nullptr;
			if (Step.NumTriangles() > 0)
			{
				Comp = NewObject<UProceduralMeshComponent>(this);
				Comp->SetupAttachment(GetRootComponent());
				BRR::FillLevelMesh(Comp, BRRArt::LookMesh(Step), false, MaterialFor);
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Comp->SetCastShadow(true); // grounded like the wrecks (classic has no shadows at all)
				Comp->SetVisibility(false);
				Comp->RegisterComponent();
				Components.Add(Comp);
			}
			Slot.Steps.Add(Comp);
		}
	}
}

void ABRRAnimDecor::Update(double ElapsedSeconds)
{
	const int64 Ms = int64(FMath::Max(ElapsedSeconds, 0.0) * 1000.0);
	for (FSlot& Slot : Slots)
	{
		if (Slot.Steps.IsEmpty()) continue;
		// LevelAnim::step_at, in whole milliseconds.
		const int32 Step = Slot.StepMs > 0 ? int32((Ms / Slot.StepMs) % Slot.Steps.Num()) : 0;
		if (Step == Slot.Shown) continue;
		if (Slot.Steps.IsValidIndex(Slot.Shown) && Slot.Steps[Slot.Shown]) Slot.Steps[Slot.Shown]->SetVisibility(false);
		if (Slot.Steps[Step]) Slot.Steps[Step]->SetVisibility(true);
		Slot.Shown = Step;
	}
}
