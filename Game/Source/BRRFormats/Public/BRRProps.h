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
	enum class EPropKind : uint8 { None, Tyres, Cone, Haybale, Fence, Barrier, Lamp, Pole, Rock, Sign, Building, Structure };

	// What a decor shape (FBRRPlacement::Shape) is, as furniture.
	BRRFORMATS_API EPropKind PropKindOf(const FString& Shape);

	// The rebuilt furniture of a level in one mesh (flat colours, and the signs' own art), and
	// which decor triangles it stands for (in section order, as FindTrees marks its own).
	BRRFORMATS_API FBRRMesh BuildProps(const FBRRWorld& World, TBitArray<>& OutReplaced, int32& OutCount);
}
