#include "BRRAnimDecor.h"

#include "BRRLevelMaterials.h"
#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

ABRRAnimDecor::ABRRAnimDecor()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void ABRRAnimDecor::Build(const FBRRWorld& World, const FString& Level)
{
	static const IConsoleVariable* Look = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.Look"));
	LookSet = Look && Look->GetInt() != 0 ? 1 : 0;
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
			// Both looks' poses, so F5 switches by visibility alone: classic the original mesh
			// exactly, remastered the rounded one.
			for (int32 L = 0; L < 2; ++L)
			{
				UProceduralMeshComponent* Comp = nullptr;
				if (Step.NumTriangles() > 0)
				{
					Comp = NewObject<UProceduralMeshComponent>(this);
					Comp->SetupAttachment(GetRootComponent());
					BRR::FillLevelMesh(Comp, L == 0 ? Step : BRR::SubdivideSmooth(Step, 2, 50.f), false, MaterialFor);
					Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Comp->SetCastShadow(true); // grounded like the wrecks (classic has no shadows at all)
					Comp->SetVisibility(false);
					Comp->RegisterComponent();
					Components.Add(Comp);
				}
				Slot.Steps[L].Add(Comp);
			}
		}
	}
}

void ABRRAnimDecor::SetLook(bool bRemastered)
{
	const int32 Want = bRemastered ? 1 : 0;
	if (Want == LookSet) return;
	for (FSlot& Slot : Slots)
	{
		if (Slot.Steps[LookSet].IsValidIndex(Slot.Shown) && Slot.Steps[LookSet][Slot.Shown]) Slot.Steps[LookSet][Slot.Shown]->SetVisibility(false);
		if (Slot.Steps[Want].IsValidIndex(Slot.Shown) && Slot.Steps[Want][Slot.Shown]) Slot.Steps[Want][Slot.Shown]->SetVisibility(true);
	}
	LookSet = Want;
}

void ABRRAnimDecor::Update(double ElapsedSeconds)
{
	const int64 Ms = int64(FMath::Max(ElapsedSeconds, 0.0) * 1000.0);
	for (FSlot& Slot : Slots)
	{
		if (Slot.Steps[LookSet].IsEmpty()) continue;
		// LevelAnim::step_at, in whole milliseconds.
		const int32 Step = Slot.StepMs > 0 ? int32((Ms / Slot.StepMs) % Slot.Steps[LookSet].Num()) : 0;
		if (Step == Slot.Shown) continue;
		if (Slot.Steps[LookSet].IsValidIndex(Slot.Shown) && Slot.Steps[LookSet][Slot.Shown]) Slot.Steps[LookSet][Slot.Shown]->SetVisibility(false);
		if (Slot.Steps[LookSet][Step]) Slot.Steps[LookSet][Step]->SetVisibility(true);
		Slot.Shown = Step;
	}
}
