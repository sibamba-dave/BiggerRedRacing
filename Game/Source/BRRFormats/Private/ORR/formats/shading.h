// DEFAULT.CG - the original's distance-shading table, and what a modern renderer keeps of it.
//
// This engine reproduces the RESULT (a continuous brighten-towards-the-camera) rather than the
// MECHANISM (a per-object palette-index remap through a 256-level LUT).
//
// The one thing to know here: a palette index is not a colour, it is a rung on a 16-entry ramp.
// Index 31 on CITY4 is not "black", it is "the darkest rung of a grey ramp" - the original walks
// it up to (80,80,80) when the face is near the camera. Painting it raw, as this engine did
// until now, is what made every tunnel interior solid black.
#pragma once

#include "threedw.h" // RGB

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bigred {

// DEFAULT.CG is 258 rows of 256 bytes: rows 0-1 are a header, row (2 + level) is the index->index
// remap for shading level `level`. Level 0 is the identity (far away = the raw authored colour);
// level 255 is the brightest (right under the camera).
constexpr size_t kShadeRowSize = 256;
constexpr int kShadeHeaderRows = 2;
constexpr int kShadeLevels = 256;
constexpr size_t kShadeTableSize = (kShadeHeaderRows + kShadeLevels) * kShadeRowSize; // 66048

// The remap row for `level` (0..255), or nullptr if `cg` is not a valid DEFAULT.CG.
const uint8_t* shade_row(const uint8_t* cg, size_t cg_size, int level);

// `palette` re-mapped through the brightest shading level - i.e. the colour each index takes on
// when it is right under the camera. Returns a copy of `palette` unchanged if `cg` is invalid,
// so a missing DEFAULT.CG degrades to "no lighting" rather than to no colours at all.
std::vector<RGB> brighten_palette(const std::vector<RGB>& palette, const uint8_t* cg, size_t cg_size);

// The radius of the lighting, in terrain cells - our reparametrisation of the original's view distance.
//
// In the original this is NOT a constant: the game retunes it every frame against the frame
// time, between 10 and 22 cells, so the 1996 build traded draw distance for framerate as it ran.
// There is no authored value to recover; it is a choice here, live-adjustable with F/G.
//
// ⚠️ We changed the SHAPE of the curve, not just the number. The original computes
// `level = ((cells-2)*0x200 - depth) >> 4` clamped to 0..255, which saturates at BOTH ends:
// colour goes raw at (cells-2)*512, and everything nearer than (cells-2)*512 - 255*16 sits at
// full brightness in a flat plateau with no gradient in it. That couples two things that should
// not be coupled - shortening the range first eats the plateau (good), then starts capping the
// peak intensity (not what "shorter" should mean). The two coincide at exactly 10 cells, where
// the plateau is 16 units and the peak is still 1.0.
//
// So we take that 10-cell case as the shape and scale it: the ramp runs from full brightness at
// the camera to raw colour at `radius`, linearly, with no plateau at either end and the peak
// intensity left alone. Range is then F/G, intensity is H/J, and they no longer interfere.
// A legacy render mode wants the original's saturating form back, plateau included.
//
// 6 cells = 2048 world units, and that number comes from the tunnels rather than from taste.
// A CITY4 tunnel is 5 pieces spanning 5120 world units (measured from MAP.DAT: spacings
// 1024/1536/1536/1024, two parallel bores). Anything near that length lights the whole tunnel at
// once and reads as no falloff at all - which is exactly what 15 cells (6656, longer than the
// tunnel) and then 9 cells (3584, still 70% of it) both did. 2048 is 40% of a tunnel: the
// gradient fits inside the bore, so the mouth is lit and the depth is not.
constexpr int kShadeViewDistanceCellsDefault = 6;

// How far up the ramp to actually go, and how dark a colour has to be to be taken up it at all.
// NEITHER of these exists in the original, which walks every index all the way to the top - and
// that is exactly the problem: it moves 184 of the 256 indices, so a faithful port brightens the
// entire near field and washes it out. On a 1996 rasterizer that reads as atmosphere; on a
// modern one, with smooth gradients and no dithering, it reads as a bug. User call, 2026-09-14.
//
// So the correction is aimed at what was actually wrong: an index sitting on the darkest rung of
// its ramp renders as flat black (the tunnel interiors, index 31 - the whole reason this system
// was studied in the first place), and flat black is a colour the original never showed up close.
// Anything already bright enough to read is left alone. `kShadeDarkCeilDefault` is the luminance,
// 0..1, above which the effect has faded out entirely.
//
// Both are live-adjustable (main.cpp's H/J and B/N keys) because they are taste, not data.
constexpr float kShadeStrengthDefault = 1.0f;
constexpr float kShadeDarkCeilDefault = 0.12f;
// Measured on the real palettes at that ceiling: the effect drops from 184 palette indices to
// ~55, and on the indices the TERRAIN actually uses it goes from 24/26 to 4/26 on CITY4, 18/20
// to 2/20 on CHOPPER1, and 13/15 to 0/15 on SNOW1. In other words it stops being a global
// wash and becomes what it was meant for: the near-black rungs, which is the tunnels.

// The distance, in RAW WORLD UNITS, at which the brightening has faded out completely. Full
// brightness is at 0 and it falls off linearly to here - see the note above on why this is a
// straight ramp and not the original's saturating one. Same base number as the original's
// (cells - 2) * 0x200. (The original uses cells*512, without the -2, for queue categories 3 and
// 8; we apply the general case to everything - see the spec.)
float shade_radius_world(int view_distance_cells);

} // namespace bigred
