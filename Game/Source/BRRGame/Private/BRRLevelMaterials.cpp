#include "BRRLevelMaterials.h"

#include "Materials/MaterialInterface.h"

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
