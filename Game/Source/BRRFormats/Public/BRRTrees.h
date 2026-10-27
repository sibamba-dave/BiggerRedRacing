#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// The remastered look's trees: the original's (flat lollipops, a few dozen triangles each) found in
// the level's decor and grown again, procedurally, where they stood - as tall, as wide and in their
// colours. The classic look and the race keep the original's, which still collide.
namespace BRR
{
	enum class ETreeKind : uint8 { None, Pine, SnowPine, Palm, Broadleaf, Bonsai, Bush, Reeds, Cactus };

	// What a decor shape (FBRRPlacement::Shape) is, as a plant.
	BRRFORMATS_API ETreeKind TreeKindOf(const FString& Shape);

	struct FBRRTree
	{
		ETreeKind Kind = ETreeKind::None;
		FVector3f Base;       // UE cm, the foot of the trunk
		float Height = 500;   // cm
		float Radius = 200;   // the crown's, cm
		float Yaw = 0;        // degrees
		FColor Crown = FColor(60, 110, 40), Trunk = FColor(90, 60, 35);
		int32 Seed = 0;
	};

	// The level's plants: each decor piece (a connected run of triangles) goes to the placement
	// nearest it; a placement of a plant shape is a tree, its pieces measured for its size and
	// colours. OutPlantTriangles marks the decor's triangles that are plants (in section order).
	BRRFORMATS_API TArray<FBRRTree> FindTrees(const FBRRWorld& World, TBitArray<>& OutPlantTriangles);

	// One tree, in flat vertex colours (no atlas), around its base.
	BRRFORMATS_API FBRRMesh GrowTree(const FBRRTree& Tree);

	// The decor without its plant triangles.
	BRRFORMATS_API FBRRMesh WithoutPlants(const FBRRMesh& Decor, const TBitArray<>& PlantTriangles);
}
