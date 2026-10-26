// MAP.DAT (scenery instance placement) + MAP.SHL (shape list) + MAP.GRP (grouping).
//
//
// Raw MAP.DAT record, 22 bytes, EVERY byte accounted for (confirmed in the original's
// level object-instance loader, and cross-validated against real data):
//   0      int8   shape_index -> MAP.SHL
//   1-12   int32  x, y, z     -> raw world units, absolute (NOT a ground-snap hint)
//   13-18  int16  angle_x, angle_y, angle_z -> BAM, full turn = 65536
//   19     uint8  texture-variant selector - on CHECKP, the checkpoint line's ORDINAL instead
//   20     uint8  collision/behaviour category (NOT shape identity)
//   21     uint8  per-category parameter (NOT a scale - no scale field exists): the gate
//                 window's end on CHECKP, `anim` slot on an animated category, unused elsewhere
//
// Two rules that are easy to get wrong and are general, without exception:
//   - Y is the instance's own absolute coordinate. No per-category height override exists;
//     the one that was tried was the bug, not the fix (the terrain height scale was).
//   - rotation_matrix() below is ported from the original, which has +Y DOWN. A renderer
//     with +Y up must flip Y before the multiply and back after - see main.cpp's to_world().
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bigred {

struct SceneryInstance {
    float x = 0, y = 0, z = 0; // raw world units - CONFIRMED, see header comment
    int shape_index = -1;       // CONFIRMED - index into the level's MAP.SHL shape list, -1 if
                                 // out of range (shouldn't happen on real data)
    int group_index = -1;       // index into the level's MAP.GRP groups, -1 if ungrouped
    uint8_t collision_category_byte = 0; // file offset 20 - see header comment, NOT shape identity
    // File offset 19 - CONFIRMED texture-variant selector, see header. On CHECKP, which is never
    // drawn, it is the line's ORDINAL in the lap, bit 0x80 on the last one (the original's gate
    // test reads it) - see sim/race.h's CheckLine.
    uint8_t variant_index = 0;
    // File offset 21. Not a scale, and not one thing: it is a per-category parameter slot. On
    // CHECKP it goes to the vehicle's END OF THE GATE WINDOW - roughly the gate of
    // the NEXT line, which is why it reads "a few gates ahead" of the line's own gate.
    // On an ANIMATED category (see
    // level_script.h's anim_slot_of) it selects which `anim` line of the level script rewrites
    // this instance's variant byte, modulo 10. Zero on the ~90% of instances whose category
    // gives it no meaning.
    uint8_t aux_byte = 0;

    // Raw BAM angles (full turn = 65536), file offset 13-14/15-16/17-18 - CONFIRMED rotation,
    // see header comment. angle_y is the vertical (yaw/heading) axis.
    int16_t angle_x = 0, angle_y = 0, angle_z = 0;

    // Builds the row-major 3x3 rotation matrix Rx(angle_x) * Ry(angle_y) * Rz(angle_z), exactly
    // matching the original's fixed-point formula (see header comment).
    void rotation_matrix(float out[9]) const {
        constexpr float kBamToRad = 6.283185307179586f / 65536.0f;
        float a = static_cast<float>(angle_x) * kBamToRad;
        float b = static_cast<float>(angle_y) * kBamToRad;
        float c = static_cast<float>(angle_z) * kBamToRad;
        float sa = std::sin(a), ca = std::cos(a);
        float sb = std::sin(b), cb = std::cos(b);
        float sc = std::sin(c), cc = std::cos(c);
        out[0] = sb * sc * sa + cb * cc;
        out[1] = cb * sc - sb * cc * sa;
        out[2] = ca * sb;
        out[3] = -ca * sc;
        out[4] = ca * cc;
        out[5] = sa;
        out[6] = cb * sc * sa - sb * cc;
        out[7] = -(cb * cc * sa + sb * sc);
        out[8] = ca * cb;
    }
};

// `dat_data`/`dat_size`: the level's raw MAP.DAT. `grp_data`/`grp_size`: the level's raw
// MAP.GRP (optional - pass nullptr/0 to skip group assignment).
std::vector<SceneryInstance> parse_map_dat(const uint8_t* dat_data, size_t dat_size,
                                            const uint8_t* grp_data, size_t grp_size);

// MAP.SHL - "Shape List": which object type names (matching <NAME>.FT/<NAME>.3DW pairs in the
// same level archive) this level uses, in archive order - directly indexed by
// SceneryInstance::shape_index. 13 bytes/entry, null-padded ASCII.
std::vector<std::string> parse_map_shl(const uint8_t* shl_data, size_t shl_size);

// ------------------------------------------------------------------ collision categories
//
// The raw category byte is looked up in the engine's 37-entry table and its
// POSITION is the internal index every switch in the game dispatches on - the collision
// router, the per-frame decor updater, the animation slot picker.
// Returns -1 for a byte the table does not hold.
int collision_category_index(uint8_t raw_category);

// Indices 4, 5 and 9: contact does not push the car back, it WRECKS the object. Confirmed in
// the original's collision router.
bool category_is_destructible(int category_index);
// Categories 0x13/0x14/0x1b/0x1c: the tunnels - never a floor, and the only
// scenery the distance lighting touches.
bool category_is_tunnel(int category_index);

// ------------------------------------------------------------------ the destruction state
//
// An object is wrecked by having its variant byte (MAP.DAT offset 19) rewritten, and that byte
// is what the shape's display list branches on - so the whole thing is one number.
//
// Two different rewrites, both as in the original:
//   - index 4 just ORs in bit 0x40 and keeps the instance's own low bits. No animation.
//   - indices 5 and 9 hand the object to a 20-slot list that a
//     per-frame routine walks: the byte is set to 0x40 and then counts up one per 14 ms
//     game tick to 0x5f, at which point it SETTLES at 0x60 and leaves the list. The shape's
//     display list compares that count against a ladder of thresholds, so the 32 steps are a
//     flipbook - ten frames on VMARKER - and 0x60 is the state it keeps forever after.
//     ⚠️ When the 20 slots are full the 21st object skips straight to 0x60: no animation.
constexpr int kWreckAnimSteps = 0x20;          // 0x40..0x5f, one per 14 ms tick
constexpr uint8_t kWreckAnimFirst = 0x40;
constexpr uint8_t kWreckSettled = 0x60;

// The variant byte a wrecked instance carries at destruction step `step` (0 = the instant of
// the crash, kWreckAnimSteps and beyond = settled).
uint8_t wrecked_variant(uint8_t variant, int category_index, int step);

} // namespace bigred
