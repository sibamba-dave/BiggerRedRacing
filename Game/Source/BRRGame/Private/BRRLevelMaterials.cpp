#include "BRRLevelMaterials.h"

#include "BRRWorld.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

UMaterialInterface* BRR::LoadLevelMaterial(const FString& Level, int32 Atlas, bool bTwoSided)
{
	// The importer's names (BRRImporter MaterialFor).
	const FString Name = FString::Printf(TEXT("MI_%s_%s%s"), *Level,
		Atlas == INDEX_NONE ? TEXT("Flat") : *FString::Printf(TEXT("Atlas%02d"), Atlas), bTwoSided ? TEXT("_2S") : TEXT(""));
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("/Game/BRR/%s/Materials/%s.%s"), *Level, *Name, *Name));
	if (!Material) UE_LOG(LogTemp, Warning, TEXT("%s missing - re-import %s"), *Name, *Level);
	return Material;
}

void BRR::FillLevelMesh(UProceduralMeshComponent* Comp, const FBRRMesh& Mesh, bool bCollision,
	TFunctionRef<UMaterialInterface*(int32 Atlas, bool bTwoSided)> MaterialFor)
{
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
			const FVector3f A = Section.Vertices[T].Position;
			const FVector Normal(((Section.Vertices[T + 2].Position - A) ^ (Section.Vertices[T + 1].Position - A)).GetSafeNormal());
			for (int32 K = 0; K < 3; ++K)
			{
				const FBRRVertex& V = Section.Vertices[T + K];
				Triangles.Add(Positions.Add(FVector(V.Position)));
				Normals.Add(Section.Vertices[T + K].Normal.IsNearlyZero() ? Normal : FVector(Section.Vertices[T + K].Normal));
				UVs.Add(FVector2D(V.UV));
				Colors.Add(V.Color);
			}
		}
		Comp->CreateMeshSection(S, Positions, Triangles, Normals, UVs, Colors, {}, bCollision);
		Comp->SetMaterial(S, MaterialFor(Section.Atlas, Section.bTwoSided));
	}
}
