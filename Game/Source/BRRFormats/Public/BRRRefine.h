#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// The remastered look's geometry: the original's meshes refined - smooth-shaded, subdivided,
// rounded - and the small things it scatters over the ground. The classic look and the race keep
// the original's geometry: none of this collides.
namespace BRR
{
	// Smooth shading: each vertex takes the area-weighted mean of the faces meeting at its position
	// whose normals lie within MaxAngle of its own face's (so creases sharper than that stay sharp).
	// Fills FBRRVertex::Normal.
	BRRFORMATS_API void SmoothNormals(FBRRMesh& Mesh, float MaxAngleDegrees);

	// Every triangle cut into Divisions x Divisions, its UVs, colour and surface type carried over.
	BRRFORMATS_API FBRRMesh Subdivide(const FBRRMesh& Mesh, int32 Divisions);

	// The original's faceted shapes made smooth (Loop subdivision, Levels times: each pass cuts
	// every triangle in four and moves the points towards the limit surface): a tree's blob of a
	// crown, a rock, an inflatable's tubes come out round. Edges folded more sharply than
	// CreaseAngle, open edges and edges shared by more than two faces are creases and stay sharp,
	// so a car's panels and a building's corners keep their lines. UVs and colours are carried
	// linearly; smooth-shaded at CreaseAngle.
	BRRFORMATS_API FBRRMesh SubdivideSmooth(const FBRRMesh& Mesh, int32 Levels, float CreaseAngleDegrees);

	// FBRRVertex::Land on a part that shines of itself (a lamp): the remastered look's materials
	// draw it glowing (the lava's way, MakeMesh's alpha 0).
	constexpr uint8 GlowLand = 0xFE;

	// A vehicle body given lamps (the remastered look's): two white headlamps on its nose and two
	// red tail lamps on its tail, set into the body's own surface where they fall, sized to it.
	BRRFORMATS_API void AddVehicleLamps(FBRRMesh& Body);

	// A wheel rounded: subdivided, and every point put back at the distance from the axle its
	// corners are at, so the original's six- or eight-sided tyre turns into a circle. Smooth-shaded
	// across the tread, sharp at its shoulders.
	BRRFORMATS_API FBRRMesh RoundWheel(const FBRRMesh& Wheel, const FVector& Axle, int32 Divisions);

	// The remastered look's open water apart from its land (EBRRLiquid::Water; ice and lava stay
	// in the terrain): OutLand the refined terrain, its water cells the bed (let down by
	// FBRRWorld::ShoreSinkAt) as sand; OutWater the original's water triangles (flat already) and,
	// at their level, the triangles beside them, which the let-down banks rise out of along a
	// curve. OutBed is empty: the bed is part of the land.
	BRRFORMATS_API void SplitWater(const FBRRWorld& World, const FBRRMesh& Refined, FBRRMesh& OutLand, FBRRMesh& OutWater, FBRRMesh& OutBed);

	// The scattered ground detail's shapes (FBRRScatter), white so the instance colours them: a tuft
	// of grass blades (origin at its root, about 40 cm tall), a pebble (about 10 cm across) and a
	// rock (about 1 m), each varied by Seed. Flat colour (no atlas); the vertex colour shades the
	// grass from its root to its tips.
	BRRFORMATS_API FBRRMesh MakeGrassTuft(int32 Seed);
	BRRFORMATS_API FBRRMesh MakeStone(int32 Seed, int32 Detail);

	// Where the race scatters the ground detail: kind 0 grass, 1 pebble, 2 rock, 3 snow lump.
	struct FBRRScatter
	{
		uint8 Kind = 0;
		FVector3f Location;   // UE cm, on the refined terrain
		float Yaw = 0;        // degrees
		float Scale = 1;
		FVector3f Up = FVector3f::UpVector; // the ground's normal (rocks sit on the slope)
		FLinearColor Colour;  // the ground's colour under it, linear
	};
	// Every scattered thing over the terrain within Reach cm of the track, deterministic for a
	// level: grass on grass, pebbles on earth and sand, rocks on rock and at its foot, lumps on
	// snow. Nothing on the track itself, the liquids or the moon's dust.
	BRRFORMATS_API TArray<FBRRScatter> ScatterGround(const FBRRWorld& World, double Reach);
}
