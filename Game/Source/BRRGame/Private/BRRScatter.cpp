#include "BRRScatter.h"
#include "HAL/IConsoleManager.h"

#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRScatter, Log, All);

namespace
{
	// How far from the track the detail reaches, and how far away each kind is still drawn (cm).
	constexpr double Reach = 2500.0;
	constexpr float GrassDistance = 9000.f, PebbleDistance = 6000.f, RockDistance = 30000.f;
}

ABRRScatter::ABRRScatter()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Static); // its static layers attach only to a static root
	Tags.Add(TEXT("BRRRemasterOnly"));
}

int32 ABRRScatter::NumInstances() const
{
	int32 N = 0;
	for (const UInstancedStaticMeshComponent* L : Shapes) N += L->GetInstanceCount();
	return N;
}

namespace
{
	TAutoConsoleVariable<int32> CVarGroundDetail(TEXT("brr.GroundDetail"), 3, TEXT("The remastered look's grass and stones: 0 none, 1 a quarter, 2 three fifths, 3 all (from the next race)."));
}

void ABRRScatter::Build(const FBRRWorld& World)
{
	const double Start = FPlatformTime::Seconds();
	// brr.GroundDetail: none, a quarter, three fifths, all.
	static const IConsoleVariable* Detail = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.GroundDetail"));
	const int32 Level = Detail ? FMath::Clamp(Detail->GetInt(), 0, 3) : 3;
	if (Level == 0) return;
	TArray<BRR::FBRRScatter> Items = BRR::ScatterGround(World, Reach);
	if (Level < 3)
	{
		const int32 Keep = Level == 1 ? 4 : 5; // one in four, three in five
		TArray<BRR::FBRRScatter> Kept;
		for (int32 I = 0; I < Items.Num(); ++I) if (Level == 1 ? I % Keep == 0 : I % Keep < 3) Kept.Add(Items[I]);
		Items = MoveTemp(Kept);
	}

	// One layer per shape: four grass tufts, then the three stones for each stone kind.
	auto Load = [](const TCHAR* Name) { return LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/BRR/Meshes/%s.%s"), Name, Name), nullptr, LOAD_NoWarn | LOAD_Quiet); };
	// Only the rocks cast shadows: grass and pebble shadows are invisible at their cull distances
	// but every instance would still render into the virtual shadow maps.
	struct FLayer { UStaticMesh* Mesh; float Distance; bool bShadow; TArray<FTransform> Transforms; TArray<float> Data; };
	TArray<FLayer> Plan;
	for (int32 I = 0; I < 4; ++I) Plan.Add({ Load(*FString::Printf(TEXT("SM_BRR_Grass%d"), I)), GrassDistance, false });
	for (int32 Kind = 1; Kind <= 3; ++Kind)
		for (int32 I = 0; I < 3; ++I) Plan.Add({ Load(*FString::Printf(TEXT("SM_BRR_Stone%d"), I)), Kind == 2 ? RockDistance : PebbleDistance, Kind == 2 });
	if (!Plan[0].Mesh || !Plan[4].Mesh)
	{
		UE_LOG(LogBRRScatter, Warning, TEXT("No scatter meshes in /Game/BRR/Meshes (re-import a level): no ground detail"));
		return;
	}

	uint32 Pick = 0;
	for (const BRR::FBRRScatter& Item : Items)
	{
		Pick = Pick * 1664525u + 1013904223u;
		const int32 Layer = Item.Kind == 0 ? int32(Pick >> 30) : 4 + (Item.Kind - 1) * 3 + int32((Pick >> 16) % 3);
		const FQuat Tilt = FQuat::FindBetweenNormals(FVector::UpVector, FVector(Item.Up));
		const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(Item.Yaw));
		Plan[Layer].Transforms.Add(FTransform(Tilt * Turn, FVector(Item.Location), FVector(Item.Scale)));
		Plan[Layer].Data.Append({ Item.Colour.R, Item.Colour.G, Item.Colour.B });
	}
	for (FLayer& L : Plan)
	{
		if (!L.Mesh || L.Transforms.IsEmpty()) continue;
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
		C->SetStaticMesh(L.Mesh);
		C->SetMobility(EComponentMobility::Static);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCanEverAffectNavigation(false);
		C->SetCullDistances(0, int32(L.Distance));
		C->SetCastShadow(L.bShadow);
		C->NumCustomDataFloats = 3;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(L.Transforms, false, true);
		C->PerInstanceSMCustomData = MoveTemp(L.Data);
		C->MarkRenderStateDirty();
		Shapes.Add(C);
	}
	UE_LOG(LogBRRScatter, Display, TEXT("%s: %d ground details in %.2f s"), *World.LevelName, Items.Num(), FPlatformTime::Seconds() - Start);
}
