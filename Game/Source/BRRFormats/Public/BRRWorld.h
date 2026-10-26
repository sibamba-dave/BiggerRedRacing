// UE-facing view of one Big Red Racing level and the vehicles raced on it.
//
// Built on the vendored OpenRedRacing parsers/scene builders (ORRCore), converted to UE space:
//   game (x, y, z), +Y down, world units  ->  UE (X = z, Y = x, Z = -y) * CmPerUnit
// The swap flips handedness (the original is right-handed, UE left-handed); triangle winding is
// reversed accordingly. Game yaw (x = sin, z = cos) maps 1:1 onto UE yaw.
#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"

namespace BRR
{
	// Centimetres per original world unit. The JEEP body is 155 units long, so 2.5 cm/unit gives a
	// 3.9 m jeep, 12.8 m terrain cells and ~3.3 km maps: real-world scale, which Chaos expects.
	inline constexpr double CmPerUnit = 2.5;

	inline constexpr int32 TerrainCellUnits = 512;
	inline constexpr int32 AtlasWidth = 256;
	inline constexpr int32 AtlasHeight = 192;

	// Game-frame point (world units, +Y down) to UE centimetres.
	inline FVector GameToUE(double X, double Y, double Z) { return FVector(Z, X, -Y) * CmPerUnit; }

	// BAM16 (65536 per turn) heading to UE yaw degrees.
	inline double Bam16ToDegrees(int32 Bam) { return double(Bam) * 360.0 / 65536.0; }
}

struct FBRRImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FColor> Pixels; // row-major, top row first; alpha 0 = transparent (palette index 0)
};

struct FBRRVertex
{
	FVector3f Position; // UE cm
	FVector2f UV;       // 0..1 within the section's atlas
	FColor Color;       // flat palette colour (used when the section has no atlas)
};

// Triangle list sharing one atlas page (or none) and one sidedness.
struct FBRRMeshSection
{
	int32 Atlas = INDEX_NONE; // index into FBRRWorld::Atlases, INDEX_NONE = flat colour only
	bool bTwoSided = true;
	TArray<FBRRVertex> Vertices; // 3 per triangle, UE (clockwise-front) winding
};

struct BRRFORMATS_API FBRRMesh
{
	TArray<FBRRMeshSection> Sections;

	int32 NumTriangles() const;
	FBox3f GetBounds() const;
};

struct FBRRPlacement
{
	FString Shape;          // MAP.SHL name, empty when the record has no shape
	FVector Location;       // UE cm
	FQuat Rotation;
	uint8 Variant = 0;      // display-list variant byte (MAP.DAT +19)
	uint8 Category = 0;     // collision/behaviour category byte (MAP.DAT +20)
	uint8 Param = 0;        // per-category parameter (MAP.DAT +21)
};

// One checkpoint-ring slab from MAP.CSE (see ORR sim/race.h Gate for semantics).
struct FBRRGate
{
	FVector2D Anchor;           // UE X/Y cm of the anchor verge
	int32 AngleRaw = 0;         // 1024 per turn, as stored
	double Length = 0;          // across the track, cm
	double HalfDepth = 0;       // along the track at the anchor side, cm
	double HalfDepthDelta = 0;  // added at the far side, cm
	bool bSafe = true;          // false for the level's `unsafe` gates (no respawn)
};

struct FBRRStart
{
	FVector Location; // UE cm, on the ground
	double Yaw = 0;   // UE degrees
};

struct FBRRWheel
{
	FBRRMesh Mesh;      // local to the socket
	FVector Socket;     // UE cm, relative to the body origin
	bool bRolls = false;
	bool bSteers = false;
};

// One `n.REC` ghost lap (see ORR sim/replay.h). Opponents in the original are these recordings
// replayed with rubber-banding; there is no driving AI.
struct FBRRGhostTrack
{
	int32 Slot = 0;          // the n of n.REC
	int32 Type = 0;          // 2 = 6 fields per record, 3 = 7 (flame/damage)
	int32 Character = 0;
	int32 VehicleIndex = 0;  // index into FBRRWorld::CarList
	int32 Logo = 0;
	int32 Colour = 0;
	// Per 28 ms tick: dx, dy, dz (world units, game frame), dpitch, dyaw, droll (BAM16 >> 8), extra.
	TArray<TStaticArray<int16, 7>> Records;
};

struct FBRRVehicleMesh
{
	FString Name;
	int32 Colour = 1;   // team colour 1..6
	FBRRMesh Body;      // local, UE cm
	TArray<FBRRWheel> Wheels;
};

struct BRRFORMATS_API FBRRWorld
{
	FString LevelName;
	FString ScriptText;              // <LEVEL>.TXT verbatim
	FBRRImage View;                  // VIEW.RAW, the level's presentation screen

	// Texture pages shared by every mesh below (terrain atlases, scenery pages, vehicle sheets).
	TArray<FBRRImage> Atlases;

	int32 MapWidth = 0;              // terrain cells
	int32 MapHeight = 0;
	TArray<uint8> HeightMap;         // MAP.MPH, raw
	FBRRMesh Terrain;                // UE world space
	FBRRMesh Decor;                  // every static scenery instance, baked in UE world space
	TArray<FBRRPlacement> Placements;
	TArray<FBRRGate> Gates;
	TArray<FBRRStart> Grid;          // retail six-car grid
	TArray<FString> CarList;              // vehicles raced on this level (`car` lines)
	TArray<FString> PresentationVehicles; // `car` lines that only decorate the front end (FEHELI)
	int32 Laps = 3;

	TArray<FBRRGhostTrack> Ghosts;
	int32 GhostRateMin = 0x120;      // `speed` line: replay rate bounds for rubber-banding (0x200 = 1x)
	int32 GhostRateNormal = 0x200;
	int32 GhostRateMax = 0x300;

	TArray<FBRRVehicleMesh> Vehicles;

	// Terrain height (UE Z, cm) under a UE X/Y location, bilinear over MAP.MPH.
	double GroundHeightAt(double UEX, double UEY) const;

	// Loads <Level>.LEV from DataDir (the folder holding ALWAYS.WAP) plus each named vehicle in
	// team colour `Colour`. Returns false with OutError on any failure.
	static bool Load(const FString& DataDir, const FString& Level, const TArray<FString>& VehicleNames,
		int32 Colour, FBRRWorld& Out, FString& OutError);
};
