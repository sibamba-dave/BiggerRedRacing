// Integer maths for the simulation core. Every routine here is exact and platform-independent:
// the simulation has to be bit-identical across machines for lockstep networking,
// so nothing in the physics may touch a float.
//
// The full spec holds the angle conventions and the 1024-entry tables.
#pragma once

#include <cstdint>

namespace bigred::fx {

// The original's own sine table: 1024 entries per turn, 1.15 fixed point. It is
// round(32767 * sin(2*pi*i/1024)) to within one unit everywhere, but a handful of entries
// differ by exactly 1, so it is embedded verbatim rather than regenerated - and embedding it
// also removes any dependence on the host libm, which is what determinism needs.
extern const int16_t kSinTable[1024];

// The terrain-slope table the original BUILDS AT STARTUP, so it is in no data file:
// it fills the table with `atan2(d * 16, 512)` for d = -256..255, where d is a
// difference of MAP.MPH height BYTES, 16 is the world units each byte is worth and 512 is the
// cell size. It is the slope of one cell edge, and the push-back reads two of them per point.
extern const int16_t kSlopeTable[512];

// The slope of a cell edge whose height byte changes by `dh`. Indexes kSlopeTable the way
// the original does, with the bias the original bakes into the array's base address.
inline int32_t slope16(int32_t dh) {
    if (dh < -256) dh = -256;
    if (dh > 255) dh = 255;
    return kSlopeTable[dh + 256];
}

// Angle conventions (all three exist in the original, do not mix them):
//   BAM16 - 65536 per turn: travel direction, terrain slope, .3DW/.MAP.DAT angles
//   BAM24 - 16777216 per turn, signed: the body's own pitch/yaw/roll and their rates
//   index - 1024 per turn: what the tables above are indexed by
constexpr int32_t kBam16Turn = 65536;
constexpr int32_t kBam24Turn = 16777216;

inline int table_index(int32_t bam16) { return static_cast<int>((bam16 >> 6) & 0x3FF); }

inline int32_t sin16(int32_t bam16) { return kSinTable[table_index(bam16)]; }
inline int32_t cos16(int32_t bam16) { return kSinTable[(table_index(bam16) + 256) & 0x3FF]; }

// (a * b) / 32768, truncated toward zero - as the original's multiply-divide helper does.
inline int32_t mul15(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) / 32768);
}

// (a * b) / c, 64-bit intermediate, truncated toward zero - as the original does.
inline int32_t muldiv(int32_t a, int32_t b, int32_t c) {
    if (c == 0) return 0; // the original would fault; every real script avoids it (see the
                           // steering curve's S[1] == 0 case, which is unreachable)
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) / c);
}

// atan2 in BAM16, as the original's atan2 is used: the angle whose COSINE is `x` and
// whose SINE is `y`. CORDIC, so pure integer and exact everywhere.
int32_t atan2_bam16(int32_t y, int32_t x);

// hypot, integer - as the original's.
int32_t hypot32(int32_t x, int32_t y);

// The same in three dimensions - as the original's. It halves the components until
// each fits in 15 bits before squaring them, which loses precision on long vectors; we keep the
// full 64-bit product instead, because the only consumer is the camera and the camera is not
// lockstep state.
int32_t hypot3(int32_t x, int32_t y, int32_t z);

} // namespace bigred::fx
