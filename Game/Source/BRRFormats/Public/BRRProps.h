#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// The remastered look's trackside furniture, rebuilt: the original's tyre walls, cones, hay bales,
// fences, barriers, lamp and telegraph posts and rocks made again at their place, size and colours
// (procedurally, like the trees, BRRTrees.h); its signs and billboards keep their art, crisp, on
// round metal posts in a frame. The classic look and the race keep the original's, which collide.
namespace BRR
{
	// Building: its box-like parts kept crisp and detailed (cornice, plinth, corners, a roof's
	// plant); Structure: what a car drives on or through (ramps, platforms, bridges, tunnels),
	// kept exactly as the race has it, never rounded.
	enum class EPropKind : uint8 { None, Tyres, Cone, Haybale, Fence, Barrier, Lamp, Pole, Rock, Sign, Building, Structure, Landmark, Flag };

	// What a decor shape (FBRRPlacement::Shape) is, as furniture.
	BRRFORMATS_API EPropKind PropKindOf(const FString& Shape);

	// The rebuilt furniture of a level in one mesh (flat colours, and the signs' own art), and
	// which decor triangles it stands for (in section order, as FindTrees marks its own).
	BRRFORMATS_API FBRRMesh BuildProps(const FBRRWorld& World, TBitArray<>& OutReplaced, int32& OutCount);

	// A flag's cloth, for the race to wave (the props give its pole): where the pole's top is,
	// which way the cloth hangs off it (Out, horizontal), and its size and colour.
	struct FBRRFlag
	{
		FVector3f Top;
		FVector3f Out;
		float Width = 150, Height = 100;
		FColor Colour = FColor(200, 30, 30);
	};
	BRRFORMATS_API TArray<FBRRFlag> FindFlags(const FBRRWorld& World);

	// The crowd's people (ABRRCrowd), in this game's country manner: Variant 0 overalls, plaid shirt
	// and a straw cowboy hat over a beard; 1 plaid shirt, jeans and a trucker cap; 2 overalls and a
	// mullet, a can held up; 3 sat in a lawn chair by a cooler, in a cap. Feet at the origin, facing
	// +X. OutShirt is the shirt alone, in greys for the crowd to tint; OutRest the rest in its colours.
	BRRFORMATS_API void MakeSpectator(int32 Variant, FBRRMesh& OutShirt, FBRRMesh& OutRest);
	constexpr int32 SpectatorVariants = 4;
}
