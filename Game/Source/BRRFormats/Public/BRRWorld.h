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

	// The original's sky: 16 bands of palette 239..224 above the horizon, from the top down, each
	// starting at this elevation (degrees); below the horizon 223 down to SkyGroundBandDegrees,
	// then 222. From ORR render/sky.h.
	inline constexpr float SkyBandDegrees[16] = { 38.45f, 19.91f, 14.42f, 11.12f, 8.65f, 6.73f, 5.08f, 3.71f,
		2.61f, 1.79f, 1.24f, 0.82f, 0.55f, 0.27f, 0.14f, 0.0f };
	inline constexpr float SkyGroundBandDegrees = 7.14f;

	// One terrain cell (512 world units) along UE X or Y; cells sit on multiples of it.
	inline constexpr double TerrainCellCm = 512.0 * CmPerUnit;

	// The original's view distance, retuned every frame between these against the frame rate
	// (ORR formats/shading.h); the remaster's default is ORR's middle choice.
	inline constexpr int32 ViewCellsMin = 10, ViewCellsMax = 22, ViewCellsDefault = 16;

	// The knock-over flipbook: 32 steps of the original's 14 ms tick, then the settled wreck.
	inline constexpr int32 WreckStepCount = 33;
	inline constexpr double WreckStepSeconds = 0.014;
}

struct FBRRImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FColor> Pixels; // row-major, top row first; alpha 0 = transparent (palette index 0)
};

// What a terrain surface is where it is not ground (FBRRWorld::LiquidOf). The classic look draws
// them all as the original's palette art; the remastered look shades each as what it is.
enum class EBRRLiquid : uint8 { None, Water, Ice, Lava };

// What the remastered look lays over a ground surface as its fine detail and relief (a CC0
// material, Tools/fetch_ground.py; the colour stays the original's). In the order of the
// importer's texture array, after None.
enum class EBRRGround : uint8 { None, Dirt, Grass, Sand, Snow, Rock, Asphalt, Regolith, Count };

struct FBRRVertex
{
	FVector3f Position; // UE cm
	FVector2f UV;       // 0..1 within the section's atlas
	FColor Color;       // flat palette colour (used when the section has no atlas)
	uint8 Land = 0xFF;  // terrain only: the surface type under the triangle (0-7, FBRRHandlingMetrics::SurfaceName)
	FVector3f Normal = FVector3f::ZeroVector; // smooth-shaded (BRR::SmoothNormals); zero: the face's own
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
	FBRRMesh Mesh;      // local to the socket, already in the vehicle's orientation
	FVector Socket;     // UE cm, relative to the body origin
	FVector Axle = FVector::RightVector; // rolling axis, vehicle frame
	double Radius = 0;  // cm
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
	// The cabin view (C past the third chase height): the interior (`IN_<NAME>`, section 1), local
	// like Body, with its own parts (the steering wheel); empty when the vehicle has none.
	FBRRMesh Cabin;
	TArray<FBRRWheel> CabinParts;
	int32 DashAtlas = INDEX_NONE; // the atlas page the dashboard's gauges are redrawn on (FBRRRace::DrawDashboard)
	FVector CabinEye = FVector::ZeroVector; // UE cm from the body origin (`internal`)
	double CabinPitchDown = 0;              // degrees the eye looks below the body's heading
};

// One destructible decor piece: how it stands, and the original's knock-over flipbook.
struct FBRRWreck
{
	int32 Instance = -1;       // index into the level's scenery instances (FBRRWorld::Placements)
	FBRRMesh Intact;           // UE world space
	TArray<FBRRMesh> Steps;    // the distinct poses of the flipbook, UE world space
	// Per flipbook step (BRR::WreckStepCount of them, the last being the settled wreck): index
	// into Steps, or -1 where the step looks the same as the intact piece.
	TArray<int32> StepMesh;
};

// One of the original's `{13, ...}` billboards: a camera-facing quad of a fixed WORLD size, rebuilt
// from the camera every frame (ORR LevelScene::sprite_quads).
struct FBRRSprite
{
	struct FFrame
	{
		bool bOn = false;
		int32 Atlas = INDEX_NONE;   // index into FBRRWorld::Atlases
		FVector2f UV0 = FVector2f::ZeroVector, UV1 = FVector2f::ZeroVector; // top-left .. bottom-right
	};
	FVector Centre = FVector::ZeroVector; // UE world space
	float HalfWidth = 0, HalfHeight = 0;  // cm
	// What picks the frame: a destructible (Frames[0] standing, 1 + flipbook step once knocked
	// over), else an `anim` slot (FBRRWorld::AnimStepSeconds), else Frames[0] always.
	int32 WreckInstance = -1;
	int32 AnimSlot = -1;
	TArray<FFrame> Frames;
};

struct BRRFORMATS_API FBRRWorld
{
	FString LevelName;
	FString ScriptText;              // <LEVEL>.TXT verbatim
	FBRRImage View;                  // VIEW.RAW, the level's presentation screen

	// Texture pages shared by every mesh below (terrain atlases, scenery pages, vehicle sheets).
	TArray<FBRRImage> Atlases;
	// The ground effects' sheets (dust, spray, sparks: the theme's `spot` shapes, ORR
	// particle_render), appended to the atlases after the vehicles'. The race draws its particles
	// from these pages (FBRRRace::SetParticleAtlasBase).
	int32 ParticleAtlasBase = INDEX_NONE;
	int32 ParticleAtlasCount = 0;
	// The theme throws up water (a BIGSPL or SPLASH2 spot: the WATER tracks). Its sheets' spray is
	// drawn olive and near-black, which the remastered look shows as white foam instead.
	bool bSplashes = false;

	TArray<FColor> Palette;          // the level's 256 colours (MAP.COL)
	TArray<FColor> SkyStrip;         // 18 sky colours: SkyBandDegrees order, then ground near, ground far
	// The level's `light` line: the direction the original's sun light travels (UE, unit), which
	// the original uses to shade its vehicles and the remaster to place the sun.
	FVector SunDirection = FVector(-0.65, 0.65, -0.38).GetSafeNormal();

	int32 MapWidth = 0;              // terrain cells
	int32 MapHeight = 0;
	TArray<uint8> HeightMap;         // MAP.MPH, raw
	FBRRMesh Terrain;                // UE world space
	FBRRMesh Decor;                  // every static scenery instance, baked in UE world space
	int32 GroundDecalTriangles = 0;  // decor triangles laid on the ground, lifted clear of it (Load)
	// The start gantry's lamps (S_LIGHTS, category 0x35) in each countdown state - red, red+amber,
	// amber, green - in UE world space. Not in Decor; empty if the level has none.
	TArray<FBRRMesh> StartLights;
	// Decor a vehicle can knock over (ORR LevelScene::wrecks). Not in Decor either.
	TArray<FBRRWreck> Wrecks;
	TArray<FBRRSprite> Sprites;
	// Per level-script `anim` slot, how long each step is held. Every instance on a slot is in
	// phase: the step showing T seconds after the start is floor(T / AnimStepSeconds) mod count.
	TArray<double> AnimStepSeconds;
	// Per slot, per step, what that step of the decor animation adds (a flag's pose), UE world
	// space; the faces every step shares are already in Decor (ORR LevelScene::anim_step_bufs).
	TArray<TArray<FBRRMesh>> AnimSteps;
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

	// The remastered look's terrain (BRRRefine.h): how far it lies above the original's at a UE
	// X/Y, in cm. A Catmull-Rom surface through the physics' own heights at the cell corners,
	// less the bilinear one through them - nothing at a corner, a curve between - clamped to
	// RefineClamp and eased to nothing on the track, the ramps and the liquids: the wheels, the
	// ground decals and the water keep the original's surface exactly.
	double RefinedOffsetAt(double UEX, double UEY) const;
	// The terrain subdivided Divisions x Divisions per triangle onto that surface, smooth-shaded.
	FBRRMesh RefinedTerrain(int32 Divisions) const;
	static constexpr double RefineClamp = 60.0;
	TArray<float> CornerZ;       // the physics' height (UE cm) at each cell corner, MapHeight rows of MapWidth (Load)
	TArray<float> CornerWeight;  // how much of the refinement each corner takes, 0..1 (Load)

	// How far the remastered look's ground lies below the original's at a UE X/Y, in cm, for its
	// open water (the original's has no bed): deeper by WaterDepthStep per cell out from the shore,
	// to WaterDepthMax, and the shore itself let down by ShoreSink, so the water (BRR::SplitWater)
	// reaches into the banks along the smooth surface's own contour instead of ending on the
	// original's triangle edges. Catmull-Rom between the corners; nothing at a corner on the dry
	// track, a ramp or another liquid, which keep the original's surface.
	double ShoreSinkAt(double UEX, double UEY) const;
	static constexpr double WaterDepthStep = 150.0;
	static constexpr double WaterDepthMax = 450.0;
	static constexpr double ShoreSink = 80.0;
	TArray<float> CornerSink; // per corner (Load)

	// What a terrain surface type (FBRRVertex::Land) is on this level. The original has no such
	// notion - water is a surface the boats race on, or the slippery one the cars slide into - so
	// it is read off the data (every level's surfaces against their colours and heights): the
	// WATER circuits race on their river as TRACK; elsewhere SLIPPY is the lakes and the sea,
	// except on SNOW, where it is ice, and on MOON, lava.
	EBRRLiquid LiquidOf(uint8 Land) const;

	// What a terrain triangle's ground is made of, from its surface type and the colour the original
	// paints it (Average: its texels' mean, or its flat colour). The surface types are handling, not
	// looks - TRACK is dirt on one circuit and tarmac on another, whole hillsides are CLIFFS - and the
	// palette art stylised, so the theme decides first (the moon is dust, SNOW snow), then the colour:
	// green grass, grey made ground or rock, the rest earth. Liquids and air are nothing.
	EBRRGround GroundOf(uint8 Land, FColor Average) const;
	// The mean colour of a page's opaque texels under a triangle's UVs.
	static FColor AverageTexels(const FBRRImage& Page, FVector2f A, FVector2f B, FVector2f C);

	// Loads <Level>.LEV from DataDir (the folder holding ALWAYS.WAP) plus each named vehicle in
	// team colour `Colour`. Returns false with OutError on any failure.
	static bool Load(const FString& DataDir, const FString& Level, const TArray<FString>& VehicleNames,
		int32 Colour, FBRRWorld& Out, FString& OutError);
};
