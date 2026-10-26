// Level terrain (MAP.MPH heightmap + MAP.MPC ground map + MAP.CHT remap tables + MAP.COL
// palette) -> a renderable, textured mesh.
//
// The full spec holds the tile-selection formula, the inverted V axis, the per-theme tint
// tables, and the derivation of the height scale below.
//
// Both scales are CONFIRMED:
//   - horizontal: 512 world units per texel (the original shifts a world coord by 9)
//   - vertical:   exactly 16 world units per MAP.MPH byte. Derived from the data, not by eye:
//     MAP.DAT stores each object's absolute Y, so for anything standing ON the ground,
//     instance.Y must equal the terrain height under it. Median ratio is exactly 16.000 across
//     CHOPPER1/SNOW1/CITY1/WATER1/RIG1 (16.003 on DIRT1), residual exactly 0 for
//     HELIPAD/BUILD/S_GRID. The earlier eyeballed 28 made the ground ~75% too tall, which is
//     what every "decor is mispositioned" bug was actually about.
#pragma once

#include "threedw.h" // reuses Vec3i/RGB/load_palette_rgb

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bigred {

constexpr int32_t kTerrainTexelWorldSize = 512; // CONFIRMED
constexpr float kHeightWorldScale = 16.0f;       // CONFIRMED exactly - see header comment

// Terrain texture-tile-atlas layout - CONFIRMED (cross-validated against real CHOPPER1.LEV data).
constexpr int kAtlasTileCols = 8;   // 256px / 32px
constexpr int kAtlasTileRows = 6;   // 192px / 32px
constexpr int kAtlasTilesPerAtlas = kAtlasTileCols * kAtlasTileRows; // 48
constexpr int kAtlasTileNoTextureThreshold = 192; // MAP_CHT[512+mpc_byte] >= this -> flat colour
constexpr int kMapChtTintTable0Offset = 0;        // MAP.CHT's 1st 256-byte sub-table
constexpr int kMapChtTintTable1Offset = 256;      // MAP.CHT's 2nd
constexpr int kMapChtTextureTableOffset = 512;    // MAP.CHT's 3rd

// MAP.MPC >= this marks a HOLE: the quad is not drawn at all, so you can see through the
// terrain. CONFIRMED (session 2026-09-14) in the original's own untextured quad path - both
// of its quad emitters wrap their whole queue insertion in `if (MAP.MPC[texel] < 0xf0)`,
// so a texel at or above 240 emits no geometry whatsoever. This is what cuts the tunnel mouths
// open and what clears the ground under the starting grid.
constexpr int kMapMpcHoleThreshold = 240;

struct TerrainVertex {
    float x, y, z;
    float r, g, b;   // flat-colour fallback (existing MAP.MPC->MAP.COL path), RAW palette colour
    float br, bg, bb; // the same index re-read through DEFAULT.CG's brightest shading level.
                       // The shader mixes between the two by camera distance: that IS the
                       // lighting. See shading.h.
    float u, v;       // texture-atlas UV, valid only when has_texture != 0
    float layer;       // which loaded atlas (0..N-1), valid only when has_texture != 0
    float has_texture; // 0.0 or 1.0 - blended in the shader against the flat colour
    // 1.0 where the distance lighting applies, 0.0 elsewhere. Since 2026-09-27 (the user's call)
    // only the TUNNELS are lit: outside them the near field washed out.
    float lit = 0.0f;
    // 1.0 on a ONE-SIDED decor face - no back side of its own in its shape - which the shader
    // then draws from one side only, as the original's backface culling does everywhere. That
    // is what makes MOON1's glass tubes glass. 0.0 = drawn both ways, as before.
    float cull = 0.0f;
};

// RESOLVED (session 2026-09-14). An earlier attempt derived the tunnel-mouth holes from the
// tunnel INSTANCES, punching an axis-aligned hole under each one's footprint. That was wrong
// (it also removed terrain that belongs there, and the same symptom appears under the starting
// grid, which has no tunnel). The holes are authored in the DATA, not derived: MAP.MPC == 240
// is the marker - see kMapMpcHoleThreshold above. It lands on exactly the tunnel mouths, the
// starting grids and the CITY block underpasses, one quad per opening, across every level.

// `mph_data`/`mpc_data` must each be exactly width*height bytes (256*256=65536 for every level
// seen so far). `cht_data` must be the level's raw 768-byte MAP.CHT (its 3rd 256-byte
// sub-table is the texture-tile selector). `atlas_count` is however
// many TEX*.TM atlases were actually loaded (from the level's own `texes <NAME> <N>` script
// line, see parse_texes_directive below) - tile indices resolving to an atlas >= atlas_count
// fall back to flat colour, same as an out-of-range (>=192) tile_index.
//
// Returns a flat triangle list (2 triangles per texel quad, 4 unshared vertices per quad so
// each quad can carry its own tile/atlas assignment), ready to upload to a GL vertex buffer.
// `world_to_render_scale` converts raw game-world units into whatever scale the caller's
// renderer uses (e.g. the same 1/300 factor applied to vehicle geometry).
// `height_scale` overrides kHeightWorldScale (still the default) - lets a caller re-run this
// with a different value at runtime, e.g. for interactive calibration (see main.cpp's +/- key
// handling) while the real value is still unconfirmed.
// `palette_bright` is `palette` re-mapped through DEFAULT.CG's brightest level (see
// shading.h's brighten_palette). Pass the same vector twice to disable the lighting.
std::vector<TerrainVertex> build_terrain_mesh(const uint8_t* mph_data, const uint8_t* mpc_data,
                                               const uint8_t* cht_data, size_t cht_size,
                                               int atlas_count,
                                               int width, int height,
                                               const std::vector<RGB>& palette,
                                               const std::vector<RGB>& palette_bright,
                                               float world_to_render_scale = 1.0f,
                                               float height_scale = kHeightWorldScale);

struct TexesDirective {
    std::string base_name; // e.g. "TEX11"
    int count = 0;          // e.g. 4
};

// Parses a level's own `<LEVEL>.TXT` script for its `texes <NAME> <N>` line. Returns nullopt if
// not found.
std::optional<TexesDirective> parse_texes_directive(const std::string& level_txt);

// Derives the on-disk texture atlas filenames for a texes directive, matching the confirmed
// real-file naming convention (TEX11, TEX12, TEX13, TEX14, ... - trailing number incremented
// by 1 per atlas): base_name must end in a run of ASCII digits.
std::vector<std::string> texes_atlas_filenames(const TexesDirective& texes);

} // namespace bigred
