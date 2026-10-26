// Bakes one original level (and the vehicles raced on it) into UE assets under DestRoot/<LEVEL>/:
//   Textures/T_<LEVEL>_Atlas<N>     nearest-filtered, unmipped atlas pages (the level palette)
//   Materials/MI_<LEVEL>_...       instances of the shared unlit masters in DestRoot/Materials
//   Meshes/SM_<LEVEL>_Terrain, SM_<LEVEL>_Decor, SM_<VEH>_Body, SM_<VEH>_Wheel<N>
//   L_<LEVEL>                       map: terrain, decor, the cars on the grid, a PlayerStart
// Vehicles are baked per level because the original draws them through the level's palette.
// The output is derived from the user's copy of the game and must not be distributed.
#pragma once

#include "CoreMinimal.h"

struct FBRRImportOptions
{
	FString Level;                  // "DIRT1"
	TArray<FString> Vehicles;       // empty = the level's own `car` list
	int32 Colour = 1;               // team colour 1..6
	FString DestRoot = TEXT("/Game/BRR");
};

namespace BRRImport
{
	// Imports one level. OutMapPackage receives the map's long package name ("/Game/BRR/DIRT1/L_DIRT1").
	BRRIMPORTER_API bool ImportLevel(const FBRRImportOptions& Options, FString& OutMapPackage, FString& OutError);

	// Every *.LEV in the data dir, upper-case base names.
	BRRIMPORTER_API TArray<FString> FindLevels();
}
