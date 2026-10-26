// What the race builds at runtime from a level's data, on the materials BRRImporter made for it.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UProceduralMeshComponent;
struct FBRRMesh;

namespace BRR
{
	// /Game/BRR/<Level>/Materials/MI_<Level>_Atlas<NN>[_2S] (or _Flat for INDEX_NONE); null, with a
	// warning, when the level needs re-importing.
	BRRGAME_API UMaterialInterface* LoadLevelMaterial(const FString& Level, int32 Atlas, bool bTwoSided);

	// Fills Comp with Mesh, one section per FBRRMeshSection, flat-shaded as the importer builds
	// static meshes, vertex colours as the palette bytes M_BRR_Flat decodes.
	BRRGAME_API void FillLevelMesh(UProceduralMeshComponent* Comp, const FBRRMesh& Mesh, bool bCollision,
		TFunctionRef<UMaterialInterface*(int32 Atlas, bool bTwoSided)> MaterialFor);
}
