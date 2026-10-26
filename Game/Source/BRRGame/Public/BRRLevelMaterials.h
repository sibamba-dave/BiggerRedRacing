// The materials BRRImporter makes for a level, for what the race builds at runtime.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;

namespace BRR
{
	// /Game/BRR/<Level>/Materials/MI_<Level>_Atlas<NN>[_2S] (or _Flat for INDEX_NONE); null, with a
	// warning, when the level needs re-importing.
	BRRGAME_API UMaterialInterface* LoadLevelMaterial(const FString& Level, int32 Atlas, bool bTwoSided);
}
