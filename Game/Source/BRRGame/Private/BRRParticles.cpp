#include "BRRParticles.h"

#include "BRRWorld.h"
#include "ProceduralMeshComponent.h"

ABRRParticles::ABRRParticles()
{
	Puffs = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Puffs"));
	Puffs->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Puffs->SetCastShadow(false);
	Puffs->SetVisibleInRayTracing(false); // camera-facing cards are wrong in a reflection
	SetRootComponent(Puffs);
}

void ABRRParticles::Update(const FBRRMesh& Mesh, const FString& Level, bool bStand)
{
	// One section per atlas ever seen, built once at capacity and refilled (FBRRCardBuffer): the
	// field holds at most 100 puffs, so a section never grows past its 128 cards.
	for (TPair<int32, FBRRCardBuffer>& B : Buffers) B.Value.Begin();
	for (const FBRRMeshSection& S : Mesh.Sections)
	{
		if (S.Atlas == INDEX_NONE || S.Vertices.IsEmpty()) continue;
		FBRRCardBuffer* B = Buffers.Find(S.Atlas);
		if (!B)
		{
			const int32 Section = Buffers.Num();
			SectionOf.Add(S.Atlas, Section);
			B = &Buffers.Add(S.Atlas);
			B->Create(Puffs, Section, 128);
			B->Begin();
			// The effects' own (M_BRR_Effect); an older import's ordinary page if there is none.
			UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/BRR/%s/Materials/MI_%s_Atlas%02d_FX"), *Level, *Level, S.Atlas),
				nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mat)
			{
				Mat = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/BRR/%s/Materials/MI_%s_Atlas%02d_2S"), *Level, *Level, S.Atlas),
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			Puffs->SetMaterial(Section, Mat);
		}
		for (int32 Q = 0; Q + 5 < S.Vertices.Num(); Q += 6)
		{
			FVector P[6];
			FVector2D Uv[6];
			FColor C[6];
			for (int32 K = 0; K < 6; ++K)
			{
				const FBRRVertex& V = S.Vertices[Q + K];
				P[K] = FVector(V.Position);
				Uv[K] = FVector2D(V.UV);
				C[K] = FColor(V.Color.R, V.Color.G, V.Color.B, 255); // alpha 255: no liquid
			}
			if (bStand)
			{
				// Each card is two triangles: lifted by half its height.
				double Low = P[0].Z, Mid = 0;
				for (int32 K = 0; K < 6; ++K) Low = FMath::Min(Low, P[K].Z), Mid += P[K].Z / 6;
				for (int32 K = 0; K < 6; ++K) P[K].Z += Mid - Low;
			}
			// Lit as the ground they rose from (the remastered look; the classic one is unlit):
			// facing the camera, a card's own normal is side-on to the sun and would come out dark.
			B->AddSix(P, Uv, C, FVector::UpVector);
		}
	}
	for (TPair<int32, FBRRCardBuffer>& B : Buffers) B.Value.Commit(Puffs, SectionOf[B.Key]);
}
