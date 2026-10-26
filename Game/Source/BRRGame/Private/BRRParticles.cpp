#include "BRRParticles.h"

#include "BRRWorld.h"
#include "ProceduralMeshComponent.h"

ABRRParticles::ABRRParticles()
{
	Puffs = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Puffs"));
	Puffs->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Puffs->SetCastShadow(false);
	Puffs->bUseAsyncCooking = true;
	SetRootComponent(Puffs);
}

void ABRRParticles::Update(const FBRRMesh& Mesh, const FString& Level)
{
	int32 Used = 0;
	for (const FBRRMeshSection& S : Mesh.Sections)
	{
		if (S.Atlas == INDEX_NONE || S.Vertices.IsEmpty()) continue;
		TArray<FVector> Positions;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> Colours;
		for (int32 I = 0; I < S.Vertices.Num(); ++I)
		{
			const FBRRVertex& V = S.Vertices[I];
			Positions.Add(FVector(V.Position));
			UVs.Add(FVector2D(V.UV));
			Colours.Add(FColor(V.Color.R, V.Color.G, V.Color.B, 255)); // alpha 255: no liquid
			Triangles.Add(I);
		}
		for (int32 T = 0; T + 2 < Positions.Num(); T += 3)
		{
			const FVector N = ((Positions[T + 2] - Positions[T]) ^ (Positions[T + 1] - Positions[T])).GetSafeNormal();
			Normals.Append({ N, N, N });
		}
		Puffs->CreateMeshSection(Used, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
		TObjectPtr<UMaterialInterface>& Mat = Materials.FindOrAdd(S.Atlas);
		if (!Mat)
		{
			Mat = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/BRR/%s/Materials/MI_%s_Atlas%02d_2S"), *Level, *Level, S.Atlas),
				nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		Puffs->SetMaterial(Used, Mat);
		++Used;
	}
	for (int32 K = Used; K < Sections; ++K) Puffs->ClearMeshSection(K);
	Sections = Used;
}
