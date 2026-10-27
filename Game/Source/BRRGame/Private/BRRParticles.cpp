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

void ABRRParticles::Update(const FBRRMesh& Mesh, const FString& Level, bool bStand)
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
		if (bStand)
		{
			// Each card is two triangles: lifted by half its height.
			for (int32 Q = 0; Q + 5 < Positions.Num(); Q += 6)
			{
				double Low = Positions[Q].Z, Mid = 0;
				for (int32 K = 0; K < 6; ++K) Low = FMath::Min(Low, Positions[Q + K].Z), Mid += Positions[Q + K].Z / 6;
				for (int32 K = 0; K < 6; ++K) Positions[Q + K].Z += Mid - Low;
			}
		}
		// Lit as the ground they rose from (the remastered look; the classic one is unlit): facing
		// the camera, a card's own normal is side-on to the sun and the spray and dust come out dark.
		Normals.Init(FVector::UpVector, Positions.Num());
		Puffs->CreateMeshSection(Used, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
		TObjectPtr<UMaterialInterface>& Mat = Materials.FindOrAdd(S.Atlas);
		if (!Mat)
		{
			// The effects' own (M_BRR_Effect); an older import's ordinary page if there is none.
			Mat = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/BRR/%s/Materials/MI_%s_Atlas%02d_FX"), *Level, *Level, S.Atlas),
				nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mat)
			{
				Mat = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/BRR/%s/Materials/MI_%s_Atlas%02d_2S"), *Level, *Level, S.Atlas),
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		Puffs->SetMaterial(Used, Mat);
		++Used;
	}
	for (int32 K = Used; K < Sections; ++K) Puffs->ClearMeshSection(K);
	Sections = Used;
}
