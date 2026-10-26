#include "vehicle.h"

#include <algorithm>
#include <cstdio>

namespace bigred {

static_assert(RaceRules{}.impact_damage_shift == VehicleSim::kImpactDamageShift,
              "the rules' default is the deviation VehicleSim documents");

// The airborne angular-rate ceiling, in BAM24 per tick - 0.53 degrees, 19 degrees a second.
//
// It is not invented: `0x6000` is what `cogs[5]` holds on EVERY rolling vehicle in the
// game, and that field is the original's own limit on `pitch_rate` in the air (the bound on the
// `cogs[4]` drift, in the airborne tail of its vehicle update). The only vehicles that carry 0 there are the
// aircraft and the INFLATE, whose drift is simply switched off - not ones asking to spin freely.
// So this reuses the engine's own statement of how fast a body may turn while airborne, and
// applies it to every body rather than only to the drift.
static constexpr int32_t kAirRateCap = 24576;



namespace {

// The engine's own clamps, straight out of the tail of the original's vehicle update.
constexpr int32_t kSpeedClamp = 0xFFFA;   // travel magnitude
constexpr int32_t kCompClamp = 0xB4FA;    // forward / lateral component
constexpr int32_t kCeilingY = -3000000;   // +Y is down, so this is a ceiling
constexpr int32_t kDirGuard = 3000;       // see may_accel / may_reverse

// The three global input-ramp coefficient triplets, the original's own constants.
// They are NOT per vehicle. {rise, opposite-side release, centring}, all over 256.
constexpr int32_t kRampSteer[3] = {246, 64, 240};   // steering
constexpr int32_t kRampThrottle[3] = {246, 64, 200}; // throttle (both channels)
constexpr int32_t kRampLift[3] = {253, 64, 200};     // lift
constexpr int32_t kFullScale = 256;                  // a constant in the original too - see the spec

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The original's input ramp, digital half: a geometric approach toward +/-full, with a separate coefficient
// for collapsing the opposite sign and for re-centring when nothing is held.
void ramp_axis(int32_t& v, bool pos, bool neg, const int32_t k[3], int32_t full) {
    if (pos == neg) {
        if (!pos) v = v * k[2] / 256; // neither held: centre
        return;                        // both held: frozen, exactly like the original
    }
    if (pos) {
        if (v >= 0) v = full - (full - v) * k[0] / 256;
        else v = v * k[1] / 256;
    } else {
        if (v <= 0) v = ((v + full) * k[0] / 256) - full;
        else v = v * k[1] / 256;
    }
}

// The ramp's single-sided variant, used for the aircraft collective and for the separate
// forward / reverse channels of `acceling 1`.
void ramp_single(int32_t& v, bool held, const int32_t k[3], int32_t full) {
    if (held) v = full - (full - v) * k[0] / 256;
    else v = v * k[2] / 256;
}

} // namespace

// ---------------------------------------------------------------------------- terrain queries

int32_t SimTerrain::height_fx(int32_t x_fx, int32_t z_fx,
                              int32_t* slope_x, int32_t* slope_z) const {
    if (!valid()) {
        if (slope_x) *slope_x = 0;
        if (slope_z) *slope_z = 0;
        return 0;
    }
    const int gx = static_cast<int>((x_fx >> 18) & 0xFF);
    const int gz = static_cast<int>((z_fx >> 18) & 0xFF);
    const int gx1 = (gx + 1) & 0xFF, gz1 = (gz + 1) & 0xFF;
    const int32_t fx = x_fx & 0x3FFFF, fz = z_fx & 0x3FFFF;
    auto H = [&](int cx, int cz) -> int32_t {
        if (cx >= w || cz >= h) return 0;
        // Negated on read: the game's +Y points DOWN, so higher ground is a smaller Y.
        return -static_cast<int32_t>(mph[static_cast<size_t>(cz) * w + cx]);
    };
    const int32_t n00 = H(gx, gz), n11 = H(gx1, gz1);
    // Each cell is split along a diagonal, and which half you are in picks the third corner.
    // `<< 13` is 16 (world units per height byte) * 512 (the fixed point).
    if (fx > fz) {
        const int32_t n10 = H(gx1, gz);
        // The two edges of THIS triangle, in the order the original's height probe emits them. The values are
        // already negated, so a difference taken this way rises with the ground.
        if (slope_x) *slope_x = fx::slope16(n00 - n10);
        if (slope_z) *slope_z = fx::slope16(n10 - n11);
        return (n10 << 13)
             + fx::muldiv(0x40000 - fx, (n00 - n10) << 13, 0x40000)
             + fx::muldiv(fz, (n11 - n10) << 13, 0x40000);
    }
    const int32_t n01 = H(gx, gz1);
    if (slope_x) *slope_x = fx::slope16(n01 - n11);
    if (slope_z) *slope_z = fx::slope16(n00 - n01);
    return (n01 << 13)
         + fx::muldiv(fx, (n11 - n01) << 13, 0x40000)
         + fx::muldiv(0x40000 - fz, (n00 - n01) << 13, 0x40000);
}

int SimTerrain::terrain_probe(int32_t x_fx, int32_t y_fx, int32_t z_fx, int32_t& ground_fx,
                              int32_t push[3]) const {
    int32_t slope_x = 0, slope_z = 0;
    ground_fx = height_fx(x_fx, z_fx, &slope_x, &slope_z);
    push[0] = push[1] = push[2] = 0;
    if (y_fx <= ground_fx) return 0;

    // Penetrating. The original does not push the point straight up: it first SLIDES it along
    // the two cell-edge slopes, then measures whatever penetration is left there.
    //
    // For a point `d` below a surface of slope angle a, the displacement along the surface
    // normal has a horizontal part d*sin(a)*cos(a) and a vertical part d*cos(a)^2 - and that
    // horizontal part is exactly what the original computes, once per axis, from the slope
    // table. Each multiply is the original's fixed-point one, (a*b)/0x8000.
    //
    // ⚠️ DELIBERATE DEVIATION 4 of 4 - THE ORIGINAL'S 16-BIT NARROWING IS NOT REPRODUCED. The original
    // keeps each intermediate in a 16-bit word, and these are FIXED-POINT values (x512): past a
    // penetration of 64 world units the slide WRAPS AROUND and comes out as garbage, almost
    // always close to zero. The push is then purely vertical, the bounce (which reflects about
    // the push) removes no horizontal speed, and a car driving into a steep face climbs it at
    // 130-265 units a tick with its speed intact - then the face's lip converts that speed into
    // a huge upward vy. That is the user's "hit a mountainside and get teleported high up"
    // (DIRT1, WATER1, 2026-09-24), measured by physics_bench's LIFT block. Unnarrowed, the
    // same contact slides the corner ~40 units back off a 71-degree face and the push is mostly
    // horizontal, so the car bounces off the mountain.
    //
    // The vertical term is then NOT d*cos(a)^2 but a fresh height probe at the slid position,
    // so a point resting on a slope ends up on the surface rather than one corner deep in it.
    //
    // ⚠️ ...and that fresh probe is BOUNDED to [0, depth] - the second half of deviation 4. On
    // relief that is not a single slope (a gorge, a ridge, a spike of heightmap) the slid point
    // can land under ground far higher than the one it went through, and the push then lifts the
    // corner above the surface it actually crossed: +2 000 units in one tick, measured on WATER2
    // and WATER3, whose V-shaped gorges catch a hovercraft. A push-back undoes a penetration; it
    // can never be worth more than that penetration, nor push a point further in.
    //
    // Measured over the 24 levels (LIFT block: flat out from 384 starts per level, and across
    // the track from every gate): lifts of more than 64 units in one tick go from 43 to 0 on
    // DIRT1, 77 to 0 across WATER1's banks, 20 to 0 on SNOW3, ~25 000 to ~2 000 on WATER2/3
    // (what remains is a hovercraft wedged at the bottom of a gorge the bench dropped it into).
    const int32_t depth = y_fx - ground_fx;
    if (faithful_probe) {
        // The original, step for step: `depth * sin` read back as a 16-bit word,
        // the slide read back as a 16-bit word, and no bound on the fresh probe.
        const int32_t sx = static_cast<int16_t>(fx::mul15(depth, fx::sin16(slope_x)));
        const int32_t sz = static_cast<int16_t>(fx::mul15(depth, fx::sin16(slope_z)));
        push[0] = static_cast<int16_t>(fx::mul15(sx, fx::cos16(slope_x)));
        push[2] = static_cast<int16_t>(fx::mul15(sz, fx::cos16(slope_z)));
        push[1] = y_fx - height_fx(x_fx - push[0], z_fx - push[2]);
        return -1;
    }
    const int32_t dx = fx::mul15(fx::mul15(depth, fx::sin16(slope_x)), fx::cos16(slope_x));
    const int32_t dz = fx::mul15(fx::mul15(depth, fx::sin16(slope_z)), fx::cos16(slope_z));
    push[0] = dx;
    push[1] = std::clamp(y_fx - height_fx(x_fx - dx, z_fx - dz), 0, depth);
    push[2] = dz;
    return -1;
}

int SimTerrain::land_type(int32_t x_fx, int32_t z_fx) const {
    if (!valid()) return 0;
    const int gx = static_cast<int>((x_fx >> 18) & 0xFF);
    const int gz = static_cast<int>((z_fx >> 18) & 0xFF);
    if (gx >= w || gz >= h) return 0;
    uint8_t b = mpc[static_cast<size_t>(gz) * w + gx];
    if (cht) {
        // Two remap tables, picked by the cell triangle - the same diagonal the terrain mesh and
        // the height probe use, as in the original's ground sampler.
        const int32_t fx_ = x_fx & 0x3FFFF, fz_ = z_fx & 0x3FFFF;
        b = cht[(fx_ < fz_ ? 0 : 256) + b];
    }
    return land[(b >> 4) & 0xF] & 7;
}

// One object, one point. The original's drivable probe, in the object's own frame.
//
// `lz` runs along the object's local Z and is what the surface is tilted along; `lx` across it.
// The footprint is a trapezoid: the half-depth interpolates between the two X bounds. The
// surface height is `height + lz * slope / halfDepth`, which is how `RAMP500` gains exactly the
// 500 units its name claims.
// The original's profile query for the VERTICAL pair alone: both of the profile's surfaces at a point, in
// world Y, or false when the point is outside the footprint. The two share every step of the
// footprint maths - the difference is only which pair of .FT words they interpolate.
static bool object_surfaces(const SimObject& o, int32_t px_w, int32_t py_w, int32_t pz_w,
                            int32_t& roof_out, int32_t& floor_out) {
    (void)py_w;
    const int32_t dx = px_w - o.x, dz = pz_w - o.z;
    if (dx >= 0x5801 || dx <= -0x5801 || dz >= 0x5801 || dz <= -0x5801) return false;
    const int32_t cs = fx::cos16(o.yaw_bam16), sn = fx::sin16(o.yaw_bam16);
    const int32_t lx = fx::mul15(cs, dx) - fx::mul15(sn, dz);
    const int32_t lz = fx::mul15(cs, dz) + fx::mul15(sn, dx);
    const int32_t span = o.x_high - o.x_low;
    if ((o.flags & 0x20) != 0 || span == 0) return false; // tapered: see profile_at below
    const int32_t half_depth =
        o.depth_low + fx::muldiv(o.depth_high - o.depth_low, lx - o.x_low, span);
    if (half_depth == 0) return false;
    if (lz < -half_depth || lz > half_depth) return false;
    if (!o.wide && (lx < o.x_low || lx > o.x_high)) return false;
    roof_out = o.roof_height + fx::muldiv(lz, o.roof_slope, half_depth) + o.y;
    floor_out = o.height + fx::muldiv(lz, o.slope, half_depth) + o.y - 3;
    return true;
}

// The original's profile query, in full: BOTH of a profile's surfaces at a world point, in world Y, plus how far
// the point has overshot the footprint's local-X bounds and whether it is past the footprint in
// local Z. Three callers need three different parts of it, which is why it is one function:
//
//   the RAMP GATE wants `deck`;
//   the TUNNEL wants all four - `deck` and `roof` to know whether the body is in the bore,
//   `overshoot` for the side walls, `past_z` to skip corners beyond the mouth.
//
// ⚠️ It is NOT the drivable probe (object_test_one). It does not apply that one's `-3` bias, and it
// REFUSES NOTHING but the coarse 0x5801 box: running past the footprint raises a flag, it does
// not make the point miss. Mixing the two up is how a ramp ends up with a deck three units off
// and a tunnel with a wall where its mouth should be.
struct ProfileAt {
    int32_t roof = 0;       // .FT 0x28/0x2a, world Y
    int32_t deck = 0;       // .FT 0x2c/0x2e, world Y - the drivable surface
    int32_t overshoot = 0;  // signed distance past the local-X bounds, 0 when between them
    bool past_z = false;    // |lz| ran past the half-depth
};
static bool profile_at(const SimObject& o, int32_t px_w, int32_t pz_w, ProfileAt& out) {
    const int32_t dx = px_w - o.x, dz = pz_w - o.z;
    if (dx >= 0x5801 || dx <= -0x5801 || dz >= 0x5801 || dz <= -0x5801) return false;
    const int32_t cs = fx::cos16(o.yaw_bam16), sn = fx::sin16(o.yaw_bam16);
    const int32_t lx = fx::mul15(cs, dx) - fx::mul15(sn, dz);
    const int32_t lz = fx::mul15(cs, dz) + fx::mul15(sn, dx);
    int32_t half_depth, x_lo, x_hi;
    if ((o.flags & 0x20) == 0) {
        const int32_t span = o.x_high - o.x_low;
        if (span == 0) return false;
        half_depth = o.depth_low + fx::muldiv(o.depth_high - o.depth_low, lx - o.x_low, span);
        x_lo = o.x_low;
        x_hi = o.x_high;
    } else {
        // Tapered: the footprint narrows along Z instead. One shipped user: DIRT3's MEXTUNN1
        // apron, flags 0x32 (the bench's TUNNELS block prints it).
        half_depth = o.depth_low;
        if (half_depth == 0) return false;
        const int32_t t = fx::muldiv(o.depth_high, lz, half_depth);
        x_lo = o.x_low + t;
        x_hi = o.x_high - t;
    }
    if (half_depth == 0) return false;
    out.overshoot = (lx < x_lo) ? (lx - x_lo) : (lx > x_hi ? lx - x_hi : 0);
    out.past_z = (lz > half_depth || lz < -half_depth);
    if (out.overshoot == 0) {
        out.roof = o.roof_height + fx::muldiv(lz, o.roof_slope, half_depth) + o.y;
        out.deck = o.height + fx::muldiv(lz, o.slope, half_depth) + o.y;
    } else {
        // Outside the local-X bounds the surfaces are CLAMPED to the end of the slope, the end
        // picked by the sign of lz - not interpolated.
        out.roof = o.roof_height + (lz < 0 ? -o.roof_slope : o.roof_slope) + o.y;
        out.deck = o.height + (lz < 0 ? -o.slope : o.slope) + o.y;
    }
    return true;
}

// The original's tunnel test, and it is the whole tunnel rule. ⚠️ It is NOT just a containment
// test: it also QUEUES the object (on two separate paths), which is the thing
// that was missed. Three outcomes, and a tunnel needs all three to behave:
//
//   body BELOW the deck            -> the shape is not there at all
//   body INSIDE the bore           -> `inside`, the deck IS the floor, AND the walls
//                                     and roof of the tunnel's own case apply
//   body ABOVE the roof            -> a floor ONLY if the shape has no roof (flags bit 2).
//                                     Otherwise the shape is not there and the TERRAIN governs,
//                                     which is what gives the hill over a tunnel its relief.
//
// Getting this to two branches breaks one end or the other: "always a floor" drops the car
// through the hill above the bore, "never a floor" plugs the mouth, because the heightmap is
// not dug out - the deck IS the roadway through the hill. Both were reported, in that order
// (user, 2026-09-21).
enum class TunnelRole { Absent, Floor, InBore };
static TunnelRole tunnel_role(const SimObject& o, int32_t x_w, int32_t y_w, int32_t z_w,
                              ProfileAt& p) {
    if (!profile_at(o, x_w, z_w, p)) return TunnelRole::Absent;
    if (y_w > p.deck) return TunnelRole::Absent;         // +Y is down: below the deck
    if (y_w >= p.roof) return TunnelRole::InBore;        // ...and under the roof
    return (o.flags & 0x02) != 0 ? TunnelRole::Floor : TunnelRole::Absent;
}

// THE RAMP GATE - the original's collision categories 0x11 and 0x19, which is the only place in the engine
// where one object is a floor or a wall depending on where you are. See SimObject::ramp_gate.
//
// Is this ramp a WALL for a body whose centre is at (x, y, z)? Outside its footprint it is
// neither, and the caller then treats it as absent on both paths.
static bool ramp_is_wall(const SimObject& o, int32_t x_w, int32_t y_w, int32_t z_w) {
    ProfileAt p;
    if (!profile_at(o, x_w, z_w, p)) return false;
    return y_w >= p.deck;   // +Y is DOWN: at or below the deck means under it
}

static int object_test_one(const SimObject& o, int32_t px_w, int32_t py_w, int32_t pz_w,
                           bool test_far_bound, int32_t& ground_out, int32_t push[3]) {
    const int32_t dx = px_w - o.x;
    const int32_t dz = pz_w - o.z;
    if (dx >= 0x5801 || dx <= -0x5801 || dz >= 0x5801 || dz <= -0x5801) return 0;

    const int32_t cs = fx::cos16(o.yaw_bam16), sn = fx::sin16(o.yaw_bam16);
    const int32_t lx = fx::mul15(cs, dx) - fx::mul15(sn, dz);
    const int32_t lz = fx::mul15(cs, dz) + fx::mul15(sn, dx);

    int32_t half_depth, x_lo, x_hi;
    if ((o.flags & 0x20) == 0) {
        const int32_t span = o.x_high - o.x_low;
        if (span == 0) return 0;
        half_depth = o.depth_low + fx::muldiv(o.depth_high - o.depth_low, lx - o.x_low, span);
        x_lo = o.x_low;
        x_hi = o.x_high;
    } else {
        // Tapered: the footprint narrows along Z instead. One shipped user: DIRT3's MEXTUNN1
        // apron, flags 0x32 (the bench's TUNNELS block prints it).
        half_depth = o.depth_low;
        if (half_depth == 0) return 0;
        const int32_t t = fx::muldiv(o.depth_high, lz, half_depth);
        x_lo = o.x_low + t;
        x_hi = o.x_high - t;
    }
    if (half_depth == 0) return 0;

    if (lz < -half_depth) return 0;
    if (test_far_bound && lz > half_depth) return 0;
    if (!o.wide && (lx < x_lo || lx > x_hi)) return 0;

    // The surface, in world Y. The -3 is the original's own bias.
    const int32_t top = o.height + fx::muldiv(lz, o.slope, half_depth) + o.y - 3;
    ground_out = top;
    if (py_w < top) return 1; // +Y is down: above the surface means resting on it

    // Inside it. The original's own scheme here (2026-09-27) is the SAME
    // as the terrain's push-back. The ramp angle atan2(slope, half-depth) is split
    // along the object's heading, the point SLIDES down the surface by pen*sin*cos on each
    // axis (each product read back as a 16-bit word), and the vertical push is a FRESH height
    // of the surface at the slid point - no footprint test there.
    //
    // ⚠️ This is what makes a ramp LAUNCH. The push is oblique, so apply_impact reflects the
    // velocity off it and turns speed into upward vy. It used to be the whole penetration
    // straight up with a guessed slide: the car was lifted up the ramp by position alone,
    // left its lip with no upward speed and simply dropped off it - measured against the
    // original's own ghosts on CITY1 (physics_bench SAUT): launch -7 against +117.
    const int32_t pen = py_w - top;
    const int32_t ang = static_cast<int16_t>(fx::atan2_bam16(o.slope, half_depth));
    const int32_t ax = static_cast<int16_t>(fx::mul15(sn, -ang));
    const int32_t az = static_cast<int16_t>(fx::mul15(cs, -ang));
    const int32_t tx = static_cast<int16_t>(fx::mul15(fx::sin16(ax), pen));
    const int32_t tz = static_cast<int16_t>(fx::mul15(fx::sin16(az), pen));
    push[0] = static_cast<int16_t>(fx::mul15(fx::cos16(ax), tx));
    push[2] = static_cast<int16_t>(fx::mul15(fx::cos16(az), tz));
    const int32_t sdx = dx - push[0], sdz = dz - push[2];
    // One product sum, one shift, kept as a 16-bit word - as the original does it.
    const int32_t slx = static_cast<int16_t>((static_cast<int64_t>(sdx) * cs -
                                              static_cast<int64_t>(sdz) * sn) / 32768);
    const int32_t slz = static_cast<int16_t>((static_cast<int64_t>(sdx) * sn +
                                              static_cast<int64_t>(sdz) * cs) / 32768);
    int32_t slid_depth = half_depth;
    if ((o.flags & 0x20) == 0)
        slid_depth = o.depth_low + fx::muldiv(slx - o.x_low, o.depth_high - o.depth_low,
                                              o.x_high - o.x_low);
    if (slid_depth == 0) slid_depth = half_depth;   // the original would divide by zero
    const int32_t slid_top = o.height + fx::muldiv(o.slope, slz, slid_depth) + o.y - 3;
    push[1] = py_w - slid_top;
    return -1;
}

ObjectCandidates SimTerrain::gather_candidates(int32_t x_fx, int32_t y_fx, int32_t z_fx,
                                               int32_t body_radius, const VehicleBox* body,
                                               int32_t yaw_bam16,
                                               const SimObject* const* bridges_ok,
                                               int bridges_ok_count) const {
    // The original asks its spatial grid for everything within the VEHICLE's own bounding radius,
    // and its silhouette test then keeps an object when the two bounding circles overlap:
    //
    //     |dx| < 0x1001 && |dz| < 0x1001 && dx*dx + dz*dz <= (r_vehicle + r_object)^2
    //
    // (the finer silhouette test that follows it in the original only trims the corners of that
    // circle, and never changes which objects a vehicle is driving on). The list stops at four.
    ObjectCandidates out;
    const int32_t cx = x_fx >> 9, cz = z_fx >> 9;
    for (const SimObject& o : objects) {
        const int32_t dx = cx - o.x, dz = cz - o.z;
        if (dx >= 0x1001 || dx <= -0x1001 || dz >= 0x1001 || dz <= -0x1001) continue;
        const int64_t reach = static_cast<int64_t>(body_radius) + o.radius;
        if (static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz > reach * reach) continue;
        // ⚠️ A RAMP UNDER WHICH THE BODY SITS IS NOT A FLOOR, it is a wall - see
        // SimObject::ramp_gate. The original asks this ONCE per object, at the body's own
        // position, before deciding whether to queue it at all, so it belongs here and not in
        // the per-corner probe: all eight corners have to agree on which the ramp is this tick.
        if (o.ramp_gate && ramp_is_wall(o, cx, y_fx >> 9, cz)) continue;
        // A BRIDGE is queued by its own case, and only when the body is over its deck - see
        // VehicleSim::hit_bridges.
        if (o.bridge) {
            bool ok = false;
            for (int k = 0; k < bridges_ok_count && !ok; ++k) ok = (bridges_ok[k] == &o);
            if (!ok) continue;
        }
        // The tunnel test's second job, done on the same object while it is in hand: is the body
        // BETWEEN this object's two surfaces? The profile carries a roof (.FT 0x28/0x2a) as
        // well as a floor (0x2c/0x2e), in the same linear form, and a centre that sits under
        // the one and over the other is inside a tunnel. +Y points down, so "under the roof"
        // is the LARGER Y. Nothing in the handling uses the answer - the camera does.
        // A TUNNEL decides its own role here, and it has three - the original's tunnel test, which
        // is both the containment test and the queue. `inside` is its flag, and the CAMERA's only
        // source of "we are in a tunnel": no other category sets it.
        if (o.tunnel) {
            // The original's narrow phase comes FIRST: a tube the body does not touch is not
            // there, whatever its bounding circle says. See SimTerrain::touches_box.
            if (body && !touches_box(o, cx, cz, yaw_bam16, *body)) continue;
            ProfileAt p;
            const TunnelRole role = tunnel_role(o, cx, y_fx >> 9, cz, p);
            if (role == TunnelRole::Absent) continue;   // the terrain governs here
            if (role == TunnelRole::InBore) out.inside = true;
            // ...and either way it is queued, because inside the bore the deck IS the roadway.
        }
        // ⚠️ A RAMP UNDER WHICH THE BODY SITS IS NOT A FLOOR - handled above, before this.
        out.items[out.count] = &o;
        if (++out.count == 4) break;
    }
    return out;
}

int SimTerrain::object_probe(const ObjectCandidates& cand, int32_t x_fx, int32_t y_fx,
                             int32_t z_fx, int32_t& ground_out, int32_t push[3],
                             const SimObject** who) const {
    const int32_t px_w = x_fx >> 9, py_w = y_fx >> 9, pz_w = z_fx >> 9;
    const bool test_far = cand.test_far();

    int result = 0;
    for (int i = 0; i < cand.count; ++i) {
        const SimObject& o = *cand.items[i];
        int32_t g = 0, p[3] = {0, 0, 0};
        int r;
        if (o.platform) {
            // ⚠️ DELIBERATE DEVIATION 1 of 4, and the user's own report is the evidence for it.
            // The platform
            // path has no footprint test at all, so what localises it in the original is the
            // SILHOUETTE test - the 32-direction radius table at `.FT`
            // +0x14 - which our candidate query approximates with a plain bounding circle.
            // That approximation is far too generous here: a WATER1 root is 70 units across
            // under a 247-unit hull, and a circle of `r_hull + r_root` lifts every corner of
            // the boat at once, turning a bump into a smooth 32-unit step. The user drove it:
            // "the roots no longer act as little ramps, they no longer make the boat hop".
            // Testing each CORNER against the root's own radius restores the differential lift
            // that tosses the nose, which is what the original does.
            {
                const int64_t dx = px_w - o.x, dz = pz_w - o.z;
                const int64_t rr = static_cast<int64_t>(o.radius) * o.radius;
                if (o.radius > 0 && dx * dx + dz * dz > rr) continue;
            }
            // The original's platform case (12/23), ported as it is written: push the point DOWN by the
            // object's lift, run the FULL terrain probe there, then raise the ground it returns
            // back up by the same lift. There is no footprint test on this path - being in the
            // candidate list IS the test, which is why that list has to be built the way the
            // original builds it.
            //
            // ⚠️ Two things this must not do, both of which it used to. It must not fake the
            // probe with a bare height compare: the real one produces an OBLIQUE push, and it is
            // that push that makes apply_impact reflect the velocity instead of just damping
            // `vy` - which is the difference between a root kicking the boat and the boat
            // silently stepping over it. And it must not return 1: the original ORs the probe's
            // result in, so a point ABOVE a platform yields 0 and the caller falls through to
            // the plain terrain, exactly as if the platform were not there.
            const int32_t dx = px_w - o.x, dz = pz_w - o.z;
            if (dx >= 0x5801 || dx <= -0x5801 || dz >= 0x5801 || dz <= -0x5801) continue;
            const int32_t lift = o.platform_lift << 9;
            int32_t gr = 0, pp[3] = {0, 0, 0};
            if (terrain_probe(x_fx, y_fx + lift, z_fx, gr, pp) == 0) continue;
            g = (gr - lift) >> 9;
            p[0] = pp[0] >> 9;
            p[1] = pp[1] >> 9;
            p[2] = pp[2] >> 9;
            r = -1;
        } else {
            r = object_test_one(o, px_w, py_w, pz_w, test_far, g, p);
        }
        if (r == 0) continue;
        ground_out = g;
        if (who) *who = &o;
        if (r == -1) {
            push[0] = p[0];
            push[1] = p[1];
            push[2] = p[2];
            result = -1;
        } else if (result == 0) {
            result = 1;
        }
    }
    return result;
}

// The category table itself lives in formats/scenery.h now: the RENDERER needs the same
// answers (which instances can be knocked over, and what variant byte a wrecked one carries),
// and two copies of a 37-entry table is exactly how the two sides drift apart.
static int category_index(uint8_t raw) { return collision_category_index(raw); }

// ------------------------------------------------------------------ blocking scenery contact
//
// The short version: every blocking shape carries a
// silhouette in its `.FT`, either 32 reaches around its own centre or, when
// `.FT` 0x16 is the 0xfffd sentinel, an oriented box. The vehicle carries the
// same 32-sector silhouette, and a contact is "my reach toward you plus your reach toward me is
// more than the distance between us".

void SimTerrain::build_blocker_grid() {
    // World units per texel are 512: the map spans w * 512 in X.
    const auto cells = [](int texels) {
        const int64_t span = static_cast<int64_t>(texels) * 512;
        return std::max(kGridDim, static_cast<int>((span + kGridCell - 1) / kGridCell));
    };
    grid_w = cells(w);
    grid_h = cells(h);
    grid.assign(static_cast<size_t>(grid_w) * static_cast<size_t>(grid_h), {});
    size_t dropped = 0;
    for (size_t i = 0; i < blockers.size(); ++i) {
        const SimObject& o = blockers[i];
        // An object registers in every cell its bounding radius can touch, so a query only ever
        // has to look at the single cell the body centre is in plus its neighbours.
        const int32_t r = o.radius > 0 ? o.radius : 1;
        const int gx0 = static_cast<int>((o.x - r) / kGridCell);
        const int gx1 = static_cast<int>((o.x + r) / kGridCell);
        const int gz0 = static_cast<int>((o.z - r) / kGridCell);
        const int gz1 = static_cast<int>((o.z + r) / kGridCell);
        bool placed = false;
        for (int gz = gz0; gz <= gz1; ++gz) {
            for (int gx = gx0; gx <= gx1; ++gx) {
                if (gx < 0 || gz < 0 || gx >= grid_w || gz >= grid_h) continue;
                grid[static_cast<size_t>(gz) * static_cast<size_t>(grid_w) + static_cast<size_t>(gx)]
                    .push_back(static_cast<int32_t>(i));
                placed = true;
            }
        }
        if (!placed) ++dropped;
    }
    if (dropped)
        std::fprintf(stderr, "Blocker grid %dx%d: %zu obstacle(s) off the map, never collided with.\n",
                     grid_w, grid_h, dropped);
}

namespace {

// The sector a bearing falls in, seen from a frame rotated by `yaw`. The original writes this as
// `(bearing + 0x400 - yaw) >> 0xb & 0x1f`: 0x800 BAM per sector, and the 0x400 is the half-sector
// that makes the division round to the nearest sector rather than the one below.
int sector_of(int32_t bearing_bam16, int32_t yaw_bam16) {
    return static_cast<int>(((bearing_bam16 + 0x400 - yaw_bam16) >> 11) & 0x1f);
}

// 65536 / cos(i * pi/16), i = 0..7: the original's own table.
constexpr int32_t kSectorSecant[8] = {65536, 66820, 70936, 78819, 92682, 117962, 171254, 335926};

// The original's silhouette test WHOLE: two 32-sector silhouettes, `a` and `b`, facing `bearing` (a -> b) at
// `dist`. The centre pair first; when it misses, the NEIGHBOURING pairs i = 1..6 on either
// side - a's sector +i against b's -i, then a's -i against b's +i. Those two rays leave the
// centres i sectors off the line between them and meet off it, each (dist/2)/cos long, so the
// test is `reach_a + reach_b > dist / cos(i)` and the depth is the overshoot brought back
// onto the line, `* cos(i)`. It is what makes a corner that sticks out touch before the
// centres are close: the whole silhouette collides, not the ray between the two centres.
// `normal_a` / `normal_b` are the `.FT` 0x56 bytes of the sectors that touched.
struct SilhouetteHit {
    bool hit = false;
    int32_t depth = 0;
    int sector_a = 0, sector_b = 0;
};
SilhouetteHit silhouette_contact(const int32_t* reach_a, int32_t yaw_a,
                                 const int32_t* reach_b, int32_t yaw_b,
                                 int32_t bearing, int32_t dist) {
    SilhouetteHit h;
    const int sa = sector_of(bearing, yaw_a);
    const int sb = sector_of(bearing + 0x8000, yaw_b);
    int32_t sum = reach_a[sa] + reach_b[sb];
    if (sum > dist) {
        h.hit = true;
        h.depth = sum - dist;
        h.sector_a = sa;
        h.sector_b = sb;
        return h;
    }
    for (int i = 1; i < 7; ++i) {
        const int32_t far = static_cast<int32_t>((static_cast<int64_t>(dist) * kSectorSecant[i]) >> 16);
        for (int side = 0; side < 2; ++side) {
            const int ia = (side == 0 ? sa + i : sa - i) & 0x1f;
            const int ib = (side == 0 ? sb - i : sb + i) & 0x1f;
            sum = reach_a[ia] + reach_b[ib];
            if (sum <= far) continue;
            h.hit = true;
            h.depth = static_cast<int32_t>((static_cast<int64_t>(sum - far) << 16) / kSectorSecant[i]);
            h.sector_a = ia;
            h.sector_b = ib;
            return h;
        }
    }
    return h;
}

// The body's rectangle against an object's `.FT` box (0x18..0x1e), by SEPARATING AXIS - the
// stand-in for the original's box test, whose corner classification is reaching for the same answer (see
// blocker_contact for why a straight transcription was worse). False on a separating axis;
// otherwise the smallest overlap, the heading of its axis, and which way along it the BODY has
// to go to get out. All integer, through the same sin/cos tables as everything else.
bool box_sat(const SimObject& o, int32_t x, int32_t z, int32_t yaw_bam16, const VehicleBox& body,
             int32_t& best_overlap, int32_t& best_heading, int32_t& best_sign) {
    const int32_t oyaw = o.yaw_bam16 & 0xFFFF;
    const int32_t vyaw = yaw_bam16 & 0xFFFF;
    // Each box's centre (the bounds are not symmetric) and half-extents.
    const int32_t o_cx = (o.box_xmin + o.box_xmax) / 2, o_cz = (o.box_zmin + o.box_zmax) / 2;
    const int32_t o_hx = (o.box_xmax - o.box_xmin) / 2, o_hz = (o.box_zmax - o.box_zmin) / 2;
    const int32_t b_cx = (body.xmin + body.xmax) / 2, b_cz = (body.zmin + body.zmax) / 2;
    const int32_t b_hx = (body.xmax - body.xmin) / 2, b_hz = (body.zmax - body.zmin) / 2;
    // Local +X of a frame turned by `yaw` is heading yaw + 0x4000, local +Z is heading yaw.
    // In this engine's axes a heading h is the world direction (sin h, cos h).
    const int32_t oax_x = fx::sin16(oyaw + 0x4000), oax_z = fx::cos16(oyaw + 0x4000);
    const int32_t oaz_x = fx::sin16(oyaw), oaz_z = fx::cos16(oyaw);
    const int32_t bax_x = fx::sin16(vyaw + 0x4000), bax_z = fx::cos16(vyaw + 0x4000);
    const int32_t baz_x = fx::sin16(vyaw), baz_z = fx::cos16(vyaw);
    // Centre-to-centre, world.
    const int32_t ocw_x = o.x + fx::mul15(o_cx, oax_x) + fx::mul15(o_cz, oaz_x);
    const int32_t ocw_z = o.z + fx::mul15(o_cx, oax_z) + fx::mul15(o_cz, oaz_z);
    const int32_t bcw_x = x + fx::mul15(b_cx, bax_x) + fx::mul15(b_cz, baz_x);
    const int32_t bcw_z = z + fx::mul15(b_cx, bax_z) + fx::mul15(b_cz, baz_z);
    const int32_t tx = ocw_x - bcw_x, tz = ocw_z - bcw_z;

    struct Axis { int32_t ax, az, heading; };
    const Axis axes[4] = {{oax_x, oax_z, (oyaw + 0x4000) & 0xFFFF},
                          {oaz_x, oaz_z, oyaw},
                          {bax_x, bax_z, (vyaw + 0x4000) & 0xFFFF},
                          {baz_x, baz_z, vyaw}};
    best_overlap = 0x7fffffff;
    best_heading = 0;
    best_sign = 1;
    for (const Axis& a : axes) {
        auto absm = [](int32_t v) { return v < 0 ? -v : v; };
        const int32_t ro = absm(fx::mul15(o_hx, fx::mul15(oax_x, a.ax) + fx::mul15(oax_z, a.az)))
                         + absm(fx::mul15(o_hz, fx::mul15(oaz_x, a.ax) + fx::mul15(oaz_z, a.az)));
        const int32_t rb = absm(fx::mul15(b_hx, fx::mul15(bax_x, a.ax) + fx::mul15(bax_z, a.az)))
                         + absm(fx::mul15(b_hz, fx::mul15(baz_x, a.ax) + fx::mul15(baz_z, a.az)));
        const int32_t d = fx::mul15(tx, a.ax) + fx::mul15(tz, a.az);
        const int32_t overlap = ro + rb - absm(d);
        if (overlap <= 0) return false;              // a separating axis: no contact at all
        if (overlap < best_overlap) {
            best_overlap = overlap;
            best_heading = a.heading;
            best_sign = (d >= 0) ? -1 : 1;           // push the BODY away from the object
        }
    }
    return true;
}

} // namespace

bool SimTerrain::touches_box(const SimObject& o, int32_t x, int32_t z, int32_t yaw_bam16,
                             const VehicleBox& body) {
    if (!o.use_box) return true;   // no box to test: the bounding circle stands alone
    int32_t ov = 0, hd = 0, sg = 1;
    return box_sat(o, x, z, yaw_bam16, body, ov, hd, sg);
}

ObjectContact SimTerrain::blocker_contact(const SimObject& o, int32_t x, int32_t y, int32_t z,
                                          int32_t yaw_bam16, int32_t ground,
                                          const VehicleBox& body, bool vertical_gate) const {
    ObjectContact c;
    c.object = &o;

    // --- the vertical gate ---------------------------------------------------------------------
    // +Y is DOWN. `y + body.ybottom` is the underside of the car; `o.y + o.y_top` is the top of
    // the object. Above it, you flew over. And an object whose top is under the ground line the
    // car would rest on is buried: the original refuses that contact too, which is what stops a
    // sunken post from grabbing a car driving over the hill it is buried in.
    // A SLAB has its own vertical logic, applied by the caller once the outline touches.
    const int32_t car_bottom = y + body.ybottom;
    const int32_t obj_top = o.y + o.y_top;
    if (!o.slab && vertical_gate) {
        if (car_bottom < obj_top) return c;
        if (ground + body.ybottom < obj_top) return c;
    }

    const int32_t dx = o.x - x;
    const int32_t dz = o.z - z;
    // The original's own early-out, and it is a box not a circle: 0x1001 on each axis.
    if (dx > 0x1000 || dx < -0x1000 || dz > 0x1000 || dz < -0x1000) return c;

    // ⚠️ (dx, dz), IN THAT ORDER. `fx::atan2_bam16(y, x)` takes the SINE axis first and the
    // COSINE axis second, and in this engine's frame X is the sine axis and Z the cosine one -
    // every other call site does it this way (`atan2_bam16(vlat, vfwd)`, `atan2_bam16(vx, vz)`,
    // the chase camera's `atan2_bam16(dx, dz)`).
    //
    // Passing them the other way round mirrors every bearing about the 45-degree line, so the
    // contact normal points somewhere the object is not. The user hit a tree head-on and was slid
    // SIDEWAYS past it, still moving forward: with a mirrored normal the reflection sends the car
    // along the obstacle instead of back off it, and the position push-out shoves it aside rather
    // than back. Report of 2026-09-16, WATER1.
    const int32_t bearing = fx::atan2_bam16(dx, dz) & 0xFFFF;
    c.bearing = bearing;

    if (o.use_box) {
        // --- the oriented box, by SEPARATING AXIS ------------------------------------
        //
        // The original classifies the body's four bottom corners against the four bounds and
        // pushes out by the face each one is nearest. Two things make a direct transcription go
        // wrong here, and both showed up in game:
        //
        //   * the body is a RECTANGLE, not a circle. Standing in for it with its centre grown by
        //     one scalar reach distorts which face comes out nearest on a very anisotropic shape
        //     - a billboard is 512 long and 20 thick - so the car was thrown back off the board's
        //     END when it had brushed its FACE. "The billboards block like a head-on tree even
        //     though I did not arrive head-on" (user, 2026-09-16);
        //   * but testing only whether a CORNER is inside fails the opposite way: a board 20
        //     units thick fits between the front and rear corners of a car 155 long. It gets
        //     straddled and no corner is ever inside, so the car sails through.
        //
        // Both disappear with the separating-axis test on the two rectangles. Four candidate
        // axes - each box's own two - and the one with the SMALLEST overlap is the contact
        // normal, its overlap the penetration. That is the minimum translation that separates
        // them, which is exactly what the face-classification is reaching for, and it cannot
        // straddle. All integer: dot products through the same sin/cos tables as everything else.
        int32_t best_overlap = 0, best_heading = 0, best_sign = 1;
        if (!box_sat(o, x, z, yaw_bam16, body, best_overlap, best_heading, best_sign)) return c;
        c.reach = 0;
        c.hit = true;
        c.depth = best_overlap;
        c.normal_bam16 =
            (best_sign > 0 ? best_heading : (best_heading + 0x8000)) & 0xFFFF;
        return c;
    }

    // --- the 32-sector silhouette -------------------------------------------------------------
    const int32_t dist = fx::hypot32(dx, dz);
    const int obj_sector = sector_of(bearing + 0x8000, o.yaw_bam16);
    const int veh_sector = sector_of(bearing, yaw_bam16);
    const int32_t reach = body.sector_reach[static_cast<size_t>(veh_sector)]
                        + o.sector_reach[static_cast<size_t>(obj_sector)];
    c.reach = reach;
    if (dist >= reach) return c;
    c.hit = true;
    c.depth = reach - dist;
    // Straight out along the line between the two centres. The `.FT` also stores a real surface
    // normal per sector at 0x56 (decoded into `sector_normal`, and it is genuinely the shape's
    // silhouette normal - HAYBALE's 32 bytes collapse to the four values of its own box), but
    // the ANGLE CONVENTION of those bytes is not settled: on HAYBALE the +X face reads 0xC0
    // where the game's own heading convention would say 0x40, i.e. they appear to be stored
    // negated. Rather than guess, the push here is derived from the geometry, which is exact for
    // a convex silhouette and needs no convention at all. Using the stored normal would only buy
    // a better SLIDE along a concave shape.
    c.normal_bam16 = (c.bearing + 0x8000) & 0xFFFF;
    return c;
}

bool SimTerrain::spot_blocked(int32_t x, int32_t z, int32_t yaw_bam16,
                              const VehicleBox& body) const {
    // As in the original: a shape whose .FT +0x16 is a box sentinel takes the box test, any other
    // the silhouette test, and a hit marks the spot taken unless the category is 3,
    // 4, 5, 8, 9 or 0x1e. Of our blockers that excludes the destructibles; the ramps and the
    // slabs are left out as well (HYPOTHESIS - the original's grid holds every instance, and
    // with no vertical test a bridge deck or a stand roof would read as blocking the road
    // under or over it; its own grid content is not known).
    if (grid.empty()) return false;
    const int cx = x / kGridCell, cz = z / kGridCell;
    for (int gz = cz - 1; gz <= cz + 1; ++gz)
        for (int gx = cx - 1; gx <= cx + 1; ++gx) {
            const std::vector<int32_t>* cell = grid_cell(gx, gz);
            if (!cell) continue;
            for (int32_t i : *cell) {
                const SimObject& o = blockers[static_cast<size_t>(i)];
                if (!o.hard) continue;
                if (blocker_contact(o, x, 0, z, yaw_bam16, 0, body, false).hit) return true;
            }
        }
    return false;
}

int SimTerrain::blocker_probe(int32_t x, int32_t y, int32_t z, int32_t yaw_bam16, int32_t ground,
                              const VehicleBox& body, ObjectContact* out, int max_out) const {
    int n = 0;
    if (grid.empty() || max_out <= 0) return 0;
    const int cx = static_cast<int>(x / kGridCell);
    const int cz = static_cast<int>(z / kGridCell);
    // ⚠️ THE 3x3 BLOCK, not the one cell the body is standing in. An object registers in the
    // cells its OWN bounding radius reaches, but contact happens at that radius PLUS the body's,
    // so a car can be touching an object whose cells it has already left. A 255-long BARRIER
    // (radius 267) against a cell of 2048 was exactly that: the car drove a third of the way
    // into it before the object even entered the query. The worst contact distance in the game
    // is far under one cell, so 3x3 is provably enough and costs nothing.
    for (int gz = cz - 1; gz <= cz + 1; ++gz) {
        for (int gx = cx - 1; gx <= cx + 1; ++gx) {
            const std::vector<int32_t>* cell = grid_cell(gx, gz);
            if (!cell) continue;
            // Deterministic order: cell by cell, then each cell's list in build order. No
            // sorting, no hashing - the simulation has to stay bit-identical across machines.
            for (int32_t i : *cell) {
                const ObjectContact c = blocker_contact(blockers[static_cast<size_t>(i)],
                                                        x, y, z, yaw_bam16, ground, body);
                if (!c.hit) continue;
                // An object registers in every cell its radius touches, so the 3x3 sweep can
                // meet the same one twice. Drop the repeat - applying its push twice would
                // double it.
                bool seen = false;
                for (int k = 0; k < n; ++k)
                    if (out[k].object == c.object) { seen = true; break; }
                if (seen) continue;
                if (n < max_out) out[n++] = c;
            }
        }
    }
    return n;
}

std::vector<SimObject> build_sim_objects(
    const std::vector<SceneryInstance>& instances,
    const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup) {
    std::vector<SimObject> out;
    auto s16 = [](const Blob& b, size_t off) -> int32_t {
        if (off + 1 >= b.size()) return 0;
        return static_cast<int32_t>(static_cast<int16_t>(
            static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8)));
    };

    for (size_t ii = 0; ii < instances.size(); ++ii) {
        const SceneryInstance& inst = instances[ii];
        const int idx = category_index(inst.collision_category_byte);
        // The original's own dispatch: 12 and 23 are platforms, 17-20 and 25-28 are solid.
        const bool platform = (idx == 12 || idx == 23);
        const bool solid = (idx >= 17 && idx <= 20) || (idx >= 25 && idx <= 28);
        // ⚠️ Of those, 0x13/0x14/0x1b/0x1c are TUNNELS and are never a floor - see
        // SimObject::tunnel. They are still built, because the bore's roof and walls are real
        // and the camera needs to know when the body is inside one.
        const bool tunnel = category_is_tunnel(idx);
        if (!platform && !solid) continue;
        if (inst.shape_index < 0 ||
            static_cast<size_t>(inst.shape_index) >= shape_names.size()) continue;
        const Blob* ft = ft_lookup(shape_names[static_cast<size_t>(inst.shape_index)]);
        if (!ft || ft->size() < 0x32) continue;

        SimObject o;
        o.x = static_cast<int32_t>(inst.x);
        o.y = static_cast<int32_t>(inst.y);
        o.z = static_cast<int32_t>(inst.z);
        o.yaw_bam16 = inst.angle_y;
        o.x_low = s16(*ft, 0x20);
        o.x_high = s16(*ft, 0x22);
        o.depth_low = s16(*ft, 0x24);
        o.depth_high = s16(*ft, 0x26);
        o.height = s16(*ft, 0x2c);
        o.slope = s16(*ft, 0x2e);
        o.roof_height = s16(*ft, 0x28);
        o.roof_slope = s16(*ft, 0x2a);
        o.flags = (*ft)[0x30];
        // 131/132/250/251 are bounded across their own Z only - walls and tunnel sides.
        o.wide = (idx == 19 || idx == 20 || idx == 27 || idx == 28);
        o.platform = platform;
        o.bridge = (idx == 0x12 || idx == 0x1a);
        o.past_z_lift = s16(*ft, 0x32);
        o.platform_lift = o.x_low; // the platform path reads .FT 0x20 as a height, not a bound
        o.radius = s16(*ft, 0x0c);
        // Categories 0x11/0x19 - the only categories that are a floor or a wall
        // depending on where the body is. See SimObject::ramp_gate.
        o.ramp_gate = (idx == 0x11 || idx == 0x19);
        o.tunnel = tunnel;
        o.instance_index = static_cast<int>(ii);
        // The narrow-phase box, which every shipped tunnel `.FT` carries (0x16 = 0xfffe, then
        // the footprint at 0x18..0x1e). The original tests it BEFORE its tunnel cases - see
        // SimTerrain::touches_box.
        {
            const uint16_t sentinel = static_cast<uint16_t>(
                static_cast<uint16_t>((*ft)[0x16]) | (static_cast<uint16_t>((*ft)[0x17]) << 8));
            o.use_box = sentinel >= 0xfffd;
            if (o.use_box) {
                o.box_xmin = s16(*ft, 0x18);
                o.box_xmax = s16(*ft, 0x1a);
                o.box_zmin = s16(*ft, 0x1c);
                o.box_zmax = s16(*ft, 0x1e);
            }
        }
        out.push_back(o);
    }
    return out;
}

// Which collision-category INDEX means what to the original's collision switch. Only the families
// that actually stop a car are here; the drivable ones are build_sim_objects' business and the slab
// families (2, 7, 0xf, 0x15, 0x16, 0x18, 0x21) are not ported yet.
static bool category_blocks(int idx) {
    switch (idx) {
        case 0x00: case 0x01: case 0x06: case 0x0b: case 0x0d:
        case 0x1d: case 0x1f: case 0x20: case 0x24:
            return true;
        default:
            return false;
    }
}

static bool category_destructible(int idx) { return category_is_destructible(idx); }

std::vector<SimObject> build_sim_blockers(
    const std::vector<SceneryInstance>& instances,
    const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup) {
    std::vector<SimObject> out;
    auto s16 = [](const Blob& b, size_t off) -> int32_t {
        if (off + 1 >= b.size()) return 0;
        return static_cast<int32_t>(static_cast<int16_t>(
            static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8)));
    };

    for (size_t ii = 0; ii < instances.size(); ++ii) {
        const SceneryInstance& inst = instances[ii];
        const int idx = category_index(inst.collision_category_byte);
        const bool blocks = category_blocks(idx);
        const bool destr = category_destructible(idx);
        // The RAMP family is in BOTH lists: whether it blocks is decided per tick by
        // ramp_is_wall(), not by its category. See SimObject::ramp_gate.
        const bool ramp = (idx == 0x11 || idx == 0x19);
        const bool slab = (idx == 0x02 || idx == 0x07 || idx == 0x0f || idx == 0x15 ||
                           idx == 0x21);
        if (!blocks && !destr && !ramp && !slab) continue;
        if (inst.shape_index < 0 ||
            static_cast<size_t>(inst.shape_index) >= shape_names.size()) continue;
        const Blob* ft = ft_lookup(shape_names[static_cast<size_t>(inst.shape_index)]);
        if (!ft || ft->size() < 0x76) continue;

        SimObject o;
        o.x = static_cast<int32_t>(inst.x);
        o.y = static_cast<int32_t>(inst.y);
        o.z = static_cast<int32_t>(inst.z);
        o.yaw_bam16 = inst.angle_y;
        o.radius = s16(*ft, 0x0c);
        o.y_bottom = s16(*ft, 0x04);
        o.y_top = s16(*ft, 0x06);
        o.blocker = true;
        o.destructible = destr;
        o.hard = blocks;
        o.instance_index = static_cast<int>(ii);
        o.splat_var = inst.aux_byte;
        o.ramp_gate = ramp;
        o.slab = slab;
        if (ramp) {
            // ⚠️ The gate reads the DRIVABLE profile off the same `.FT`, at 0x20-0x31, while
            // the narrow phase below reads the BLOCKING one at 0x16-0x1e. The two do not
            // overlap - the documented union starts its
            // silhouette form at 0x16 and its box form ends at 0x1e - so a ramp can carry
            // both, and it has to: the height test needs the deck, the contact needs the
            // outline.
            o.x_low = s16(*ft, 0x20);
            o.x_high = s16(*ft, 0x22);
            o.depth_low = s16(*ft, 0x24);
            o.depth_high = s16(*ft, 0x26);
            o.height = s16(*ft, 0x2c);
            o.slope = s16(*ft, 0x2e);
            o.flags = (*ft)[0x30];
        }
        // The sentinel lives at 0x16 - sector 1 of the silhouette read as a u16. A shape that
        // cannot be described by 32 radii from its own centre (a long wall, a signboard) stores
        // -1 there and four box bounds after it instead.
        const uint16_t sentinel = static_cast<uint16_t>(
            static_cast<uint16_t>((*ft)[0x16]) | (static_cast<uint16_t>((*ft)[0x17]) << 8));
        o.use_box = sentinel >= 0xfffd;
        if (o.use_box) {
            o.box_xmin = s16(*ft, 0x18);
            o.box_xmax = s16(*ft, 0x1a);
            o.box_zmin = s16(*ft, 0x1c);
            o.box_zmax = s16(*ft, 0x1e);
        } else {
            for (int i = 0; i < 32; ++i) {
                o.sector_reach[i] = s16(*ft, static_cast<size_t>(kFtSectorReach + i * 2));
                o.sector_normal[i] = (*ft)[static_cast<size_t>(kFtSectorNormal + i)];
            }
        }
        out.push_back(o);
    }
    return out;
}

// ------------------------------------------------------------------------------- the primitive

bool VehicleSim::approach(int32_t& cur, int32_t target, int32_t step) {
    const int32_t delta = target - cur;
    if (delta >= -step && delta <= step) {
        cur = target;
        return false;
    }
    cur += (delta < 0) ? -step : step;
    return true;
}

int32_t VehicleSim::gear_curve(int32_t input, const std::vector<GearRow>& rows) {
    if (rows.empty()) return 0;
    const size_t last = std::min<size_t>(rows.size(), 12) - 1;
    for (size_t i = 0; i < last; ++i) {
        if (input < rows[i + 1].speed) {
            const int32_t span = rows[i + 1].speed - rows[i].speed;
            const int32_t t = input - rows[i].speed;
            gear = rows[i].gear;
            rev = rows[i].rev_lo + fx::muldiv(rows[i].rev_hi - rows[i].rev_lo, t, span);
            return rows[i].thrust + fx::muldiv(rows[i + 1].thrust - rows[i].thrust, t, span);
        }
    }
    // Past every threshold: no thrust, and the last row's gear stands. Every shipped table ends
    // in `&7fffffff, 0, ...` precisely so this is unreachable in practice.
    gear = rows[last].gear;
    return 0;
}

// ----------------------------------------------------------------------------------- the tick

void VehicleSim::reset(const VehicleTuning& t, const SimTerrain& ter, const RaceRules& r,
                       const RespawnPoint& start, const NitroReserve& nitros) {
    reset(t, ter, start, nitros);
    rules = &r;
}

void VehicleSim::reset(const VehicleTuning& t, const SimTerrain& ter, const RespawnPoint& start,
                       const NitroReserve& nitros) {
    *this = VehicleSim();
    tune = &t;
    terrain = &ter;
    respawn = start;
    set_damage_option(damage_option);
    // The LEVEL says how much boost you get, and the VEHICLE's mode says which of the `nitros`
    // line's two numbers that is - as in the original, and this is the whole reason the second number
    // is always exactly 200x the first:
    //
    //   mode 1 (pulse)     reserve = `nitros` FIRST value  - that many boosts of `nitro[1]` ticks
    //   mode 2 (holdable)  reserve = `nitros` SECOND value - and `nitro[1]` is 1 tick, so one
    //                      unit of reserve buys one tick of boost
    //   mode 3 (aircraft)  no reserve: `nitro` is the collective, there is nothing to spend
    //
    // Both ground modes therefore grant the SAME total boost time (800 ticks = 22.4 s on DIRT1):
    // mode 1 in four fixed 5.6 s bursts, mode 2 as a trickle the player meters out. The reserve
    // only ever goes down - a spent tank stays spent for the rest of the race, and the original's
    // empty branch plays a sound and runs a separate 0x80..0xa0 flame phase instead.
    switch (t.nitro[0]) {
    case 1: nitro_fuel = nitros.count; break;
    case 2: nitro_fuel = nitros.full_scale_ticks; break;
    default: nitro_fuel = 0; break;
    }
    // A new race starts undamaged - the one place the damage is ever cleared.
    damage = 0;
    damage_percent = 0;
    wrecked_ticks = 0;
    recover();
}

void VehicleSim::set_damage_option(int32_t option) {
    // The original's own line, at race setup:
    //     capacity = hnd_damage * (100 - option) / 100, floored at 1
    // The option is the SHARE OF DAMAGE TAKEN, so it makes the car more fragile as it rises: the
    // default of 30 leaves a gauge of 70 out of a declared 100. See the declaration.
    //
    // ⚠️ This was shipped backwards once - the default was read as "the gauge is at 30%" and set
    // to 70, making every vehicle three times more brittle than the game. The option is what the
    // player dials, not what is left.
    damage_option = clamp32(option, 0, 100);
    const int32_t declared = (tune && tune->damage > 0) ? tune->damage : 1;
    damage_capacity = declared * (100 - damage_option) / 100;
    if (damage_capacity < 1) damage_capacity = 1;
}

void VehicleSim::recover(int32_t drop) {
    ++respawns;
    px = respawn.x << 9;
    pz = respawn.z << 9;
    py = (terrain ? terrain->height_fx(px, pz) : 0) - (drop << 9); // `drop` units UP (+Y is down)
    yaw = (respawn.yaw_bam16 & 0xFFFF) << 8;
    pitch = roll = 0;
    pitch_rate = yaw_rate = roll_rate = 0;
    speed = vfwd = vlat = vy = 0;
    travel = respawn.yaw_bam16 & 0xFFFF;
    boost_timer = 0;
    // ⚠️ NO REPAIR, as in the original: the damage stays. Until 2026-09-27 this repaired the
    // car - an assumed deviation from the days there was no race to end, when a wreck was a
    // dead end with nothing to reset it. A wreck cannot even ask for this any more (step()).
}

void VehicleSim::integrate() {
    // The original's integration, in its exact order.
    pitch += pitch_rate;
    yaw += yaw_rate;
    roll += roll_rate;
    // The angles are SIGNED 24-bit: the original rewrites the top byte to 0x00 or 0xFF, which is
    // precisely a sign extension from bit 23.
    auto wrap24 = [](int32_t a) {
        a &= 0x00FFFFFF;
        return (a > 0x7FFFFF) ? (a - 0x1000000) : a;
    };
    pitch = wrap24(pitch);
    yaw = wrap24(yaw);
    roll = wrap24(roll);

    py += vy;
    px += fx::mul15(speed, fx::sin16(travel));
    pz += fx::mul15(speed, fx::cos16(travel));
    // X and Z are masked to 26 bits: the world is a 256x512 torus and the original lets you
    // wrap around it.
    px &= 0x03FFFFFF;
    pz &= 0x03FFFFFF;
}

// The scenery half of the original's collision pass, plus its response pair.
//
// The response is TWO separate things, and only having both makes a wall feel like a wall:
//
//   1. the velocity is reflected about the contact normal, with the component going INTO the
//      surface halved. The tangential component is untouched, which is why you scrape along a
//      fence instead of stopping dead against it;
//   2. the POSITION is pushed straight back out along the normal by the penetration plus 4.
//      This is a positional correction, not a force - the original writes x and z directly and
//      re-derives the fixed-point position from them.
//
// ⚠️ ONE THING IS RECONSTRUCTED, not confirmed. The original halves the normal component and
// recombines it with the tangential one through the engine's atan2/length pair; whether it also
// REVERSES that component (a bounce) or merely damps it (a scrape) could not be settled from
// the original's code alone. Damping is
// implemented here: it is the reading that matches the positional push-out doing the separating,
// and a true bounce off every tree would be very noticeable. If contacts feel too sticky in
// game, this is the line to flip.
void VehicleSim::hit_scenery() {
    scenery_hit = false;
    scenery_hit_count = 0;
    // See the declaration: this is an EVENT. Leaving it set is what made one flattened sheep
    // scream for the rest of the race.
    scenery_wrecked = -1;
    just_destroyed = false;
    // ⚠️ AND SO IS THE MAGNITUDE. It is only written inside the "moving into the surface"
    // branch below, so leaving last time's value behind means every later contact - including
    // one that produces no response at all - reads as a crash of whatever the worst one was.
    // On CITY4 the grass is carpeted with destructible plants, so a car driving over flattened
    // ones raised a contact on every single tick and the audio banged on every single tick
    // (user, 2026-09-21: "le son d'impact est joue en boucle quand la voiture est sur l'herbe").
    impact_magnitude = 0;
    if (!terrain || !tune) return;
    if (terrain->blockers.empty()) return;

    const int32_t x = px >> 9, y = py >> 9, z = pz >> 9;
    const int32_t ground = terrain->height_fx(px, pz) >> 9;
    ObjectContact contacts[kMaxSceneryContacts];
    const int n = terrain->blocker_probe(x, y, z, yaw >> 8, ground, tune->box, contacts,
                                         kMaxSceneryContacts);
    if (n == 0) return;

    for (int ci = 0; ci < n; ++ci) {
        const ObjectContact& c = contacts[ci];
        // A ramp only blocks from underneath - see SimObject::ramp_gate. Asked at the body's
        // own position, exactly as the original asks it, so that this and gather_candidates()
        // can never disagree about which the ramp is on a given tick.
        if (c.object && c.object->ramp_gate && !ramp_is_wall(*c.object, x, y, z)) continue;
        // Categories 2/7/0xf/0x15/0x21 - a SLAB. Exactly the vertical logic of the
        // car-to-car case (hit_vehicles), judged on the Y the body had at the start of the tick
        // (our `snap_y`).
        if (c.object && c.object->slab) {
            const SimObject& o = *c.object;
            const VehicleBox& b = tune->box;
            const int32_t o_bottom = o.y + o.y_bottom, o_top = o.y + o.y_top;
            if (o_bottom < y + b.ytop) continue;     // wholly above the body
            if (y + b.ybottom < o_top) continue;     // the body is wholly above it
            if (o_bottom < b.ytop + snap_y) {        // it came from underneath: a ceiling
                py = static_cast<int32_t>(static_cast<uint32_t>(snap_y) << 9);
                if (vy < 0) vy = -vy;
                continue;
            }
            if (snap_y + b.ybottom < o_top) {        // it came from above: a FLOOR
                py = static_cast<int32_t>(static_cast<uint32_t>(snap_y) << 9);
                if (vy > 0) vy = -vy;
                continue;
            }
            // otherwise from the side: a wall, the ordinary blocking response below
        }
        scenery_hit_instance = c.object ? c.object->instance_index : -1;
        last_contact_car = -1;   // the last contact now names an object, not a car
        if (scenery_hit_count < 8) scenery_hit_instances[scenery_hit_count++] = scenery_hit_instance;

        // Knocking a prop over is recorded whatever else happens: bit 0x40, once, for good.
        if (c.object && c.object->destructible) {
            // `world` is the state that lasts - shared by every vehicle in the race, and what
            // the renderer draws from. The original's categories 4/5/9 act only when bit 0x40 is
            // still clear, so a prop already down is simply not wrecked a second time.
            const bool already_wrecked =
                world ? world->is_wrecked(scenery_hit_instance)
                      : (scenery_wrecked == scenery_hit_instance);
            if (world) world->wreck(scenery_hit_instance, tick);
            // ⚠️ ONLY on the tick it actually goes down. A car drives ACROSS a flattened prop
            // for several ticks after knocking it over, and reporting the event on each of them
            // replayed the impact sound two or three times per sheep (user, 2026-09-21: "le son
            // se déclenche au moins 2 fois"). `already_wrecked` was computed right above and
            // simply not consulted here.
            if (!already_wrecked) {
                scenery_wrecked = scenery_hit_instance;
                scenery_wrecked_var = c.object->splat_var;
            }
            // ⚠️ ÉCART ASSUMÉ - this is NOT what the original does, and it is here on the
            // user's word. The original's categories 4, 5 and 9 set the wreck bit, play a sound,
            // and return WITHOUT ANY PUSH: read literally, you drive clean through a road sign.
            // The user, who has played the game: "a sign or a roadside chevron pushes the car
            // away at an angle proportional to the one it arrived at", and in our build "the
            // signs don't act as a wall, you go through them". Since ROADSIGN and MSIGN1/2 are
            // exactly these destructible categories, the literal reading would leave them
            // permanently intangible.
            //
            // So a destructible pushes like anything else. The two readings can be told apart in
            // one drive: if knocking a sign over should NOT slow you at all, delete this and put
            // the `continue` back. Kept behind one flag so that is a one-line change.
            if (!kDestructiblesPushBack) continue;
            // A prop already flattened stops resisting - you drive over the wreck.
            if (already_wrecked) continue;
        }

    // ⚠️ ONLY NOW. `scenery_hit` is the original's collision flag, and it is written on the
    // BLOCKING categories alone - never on 4, 5 or 9. A destructible contact that produced no response is
    // not a collision, and reporting one is what put an impact bang under every tick spent
    // driving over an already-flattened plant.
    scenery_hit = true;

    resolve_contact(c.normal_bam16, c.depth, c.bearing);
    }
}

// The original's response pair, shared by every blocking contact: the scenery
// narrow phase above and the tunnel walls below both end here. `normal` points OUT of the
// obstacle in the world frame, `depth` is the penetration, `bearing` is where the contact is
// relative to the body (used only to place the sparks).
void VehicleSim::resolve_contact(int32_t normal, int32_t depth, int32_t bearing) {
    const int32_t x = px >> 9, y = py >> 9, z = pz >> 9;
    {
        // Angle between where the car is going and the INTO-the-surface direction. At 0 it is
        // driving squarely into the object; at +/-0x4000 it is running along the face.
        int32_t into = (travel - (normal + 0x8000)) & 0xFFFF;
        if (into > 0x8000) into -= 0x10000;
        if (into < 0x4000 && into > -0x4000) {           // moving into it at all
            // cos is the component ALONG the into-direction - the one that has to go - and sin
            // the one that runs along the face, which is kept whole. Getting these two the wrong
            // way round turns a head-on crash into a free pass and a graze into a dead stop.
            //
            // ⚠️ THE NORMAL COMPONENT IS REVERSED, not merely damped. This is a REFLECTION off
            // the contact plane at half restitution, and it is what makes an impact THROW THE CAR
            // BACK - user, 2026-09-16, who knows the game: "a car that runs into a tree is pushed
            // backwards, and a sign or a roadside chevron pushes it away at an angle proportional
            // to the angle it arrived at". That proportionality IS the reflection: the tangential
            // half is untouched, so the shallower the approach the more the car simply carries on
            // along the obstacle, and the squarer it is the more of the speed comes straight back.
            // Damping alone gave no rebound angle at all.
            const int32_t inward = -fx::mul15(speed, fx::cos16(into)) / 2;
            const int32_t along = fx::mul15(speed, fx::sin16(into));
            impact_magnitude = fx::mul15(speed, fx::cos16(into));
            if (impact_magnitude < 0) impact_magnitude = -impact_magnitude;
            // Leave the impact SPARKS a point to be born at - the original's own response, which
            // writes the contact point and raises its spark flag on exactly this branch. See the
            // declaration for what our point is and why it is not the original's.
            {
                const int32_t nose = (tune->box.zmax - tune->box.zmin) / 2;
                spark_x = x + fx::mul15(nose, fx::sin16(bearing));
                spark_z = z + fx::mul15(nose, fx::cos16(bearing));
                spark_y = y;
                spark_pending = true;
            }
            speed = fx::hypot32(along, inward);
            travel = (normal + 0x8000 + fx::atan2_bam16(along, inward)) & 0xFFFF;
        }
    }

    push_and_damage(normal, depth);
}


void VehicleSim::push_and_damage(int32_t normal, int32_t depth) {
    // 2. the position. `+ 4` is the original's own separation margin, not a fudge: without it a
    // body sitting exactly on the boundary re-triggers the contact every tick.
    //
    // ⚠️ X TAKES THE SINE AND Z THE COSINE, exactly like the integration twenty lines up
    // (`px += mul15(speed, sin16(travel))`). Swapping them mirrors the push about the 45-degree
    // diagonal, so it does not undo the penetration it was computed from - the body stays inside
    // and only crawls out over several ticks as the reflected velocity carries it. Traced on an
    // isolated ROADSIGN: first contact at 63 units deep, then 43, 23, 3 - four ticks to clear
    // what should have cleared in one. That is the user's "I go slightly through it and then the
    // physics seems to catch up", 2026-09-16.
    {
        const int32_t out = depth + 4;
        px += fx::mul15(out, fx::sin16(normal)) << 9;
        pz += fx::mul15(out, fx::cos16(normal)) << 9;
    }

    // 3. the damage, at the tail of the original's response.
    take_impact_damage(impact_magnitude);
}

void VehicleSim::take_impact_damage(int32_t magnitude) {
    // The THRESHOLD is the original's; the shift is not - see kImpactDamageShift.
    if (magnitude > 0x5db && damage >= 0) {
        damage += magnitude >> race_rules().impact_damage_shift;
        const int32_t max_damage = damage_capacity > 0 ? damage_capacity : 1;
        damage_percent = clamp32(damage * 100 / max_damage, 0, 100);
        if (damage_percent >= 100 && damage >= 0) {
            damage = -1;                                 // destroyed
            just_destroyed = true;
        }
    }
}

// The original's car-to-car contact, once per other racer, from the tail of its collision pass.
// Every figure below is the original's, the narrow phase included: the silhouette test in full,
// neighbouring sector pairs and all (silhouette_contact). Without them only the ray between the two centres was tested, and
// two cars side by side and a length apart could overlap corner to corner untouched; the
// original's cars read as "a rectangle round the whole vehicle" that pushes the other one away
// before the bodies seem to meet (user, 2026-09-24).
void VehicleSim::hit_vehicles() {
    vehicle_hit = -1;
    vehicle_hit_sound = -1;
    if (!field || !tune) return;
    for (int j = 0; j < field_count; ++j) {
        if (j == field_self) continue;
        const VehicleSim* o = field[j];
        if (!o || o == this || !o->active || !o->tune) continue;
        const int32_t x = px >> 9, y = py >> 9, z = pz >> 9;
        const int32_t dx = (o->px >> 9) - x, dz = (o->pz >> 9) - z;
        if (dx > 0x1000 || dx < -0x1000 || dz > 0x1000 || dz < -0x1000) continue;

        // --- bounding radii, then the two silhouettes --------------------------------------
        const VehicleBox& sb = tune->box;
        const VehicleBox& ob = o->tune->box;
        const int64_t d2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz;
        const int64_t rr = static_cast<int64_t>(sb.radius) + ob.radius;
        if (d2 > rr * rr) continue;
        const int32_t dist = fx::hypot32(dx, dz);
        const int32_t bearing = fx::atan2_bam16(dx, dz) & 0xFFFF;   // self -> other, X = sine
        const SilhouetteHit sh = silhouette_contact(sb.sector_reach.data(), yaw >> 8, ob.sector_reach.data(),
                                                    o->yaw >> 8, bearing, dist);
        if (!sh.hit) continue;
        const int os = sh.sector_b;
        const int32_t depth = sh.depth;

        // --- the two boxes must overlap in Y (+Y is DOWN) ---------------------------------
        const int32_t o_y = o->py >> 9;
        const int32_t o_bottom = o_y + ob.ybottom, o_top = o_y + ob.ytop;
        if (y + sb.ytop > o_bottom) continue;       // wholly under the other car
        if (o_top > y + sb.ybottom) continue;       // wholly over it
        // Where the body was at the start of the tick decides what the contact IS. It was
        // entirely under the other car: it hit the underside - back to where it was, and a
        // body going up comes down again. Entirely over it: it landed on the roof.
        if (o_bottom < sb.ytop + snap_y) {
            py = static_cast<int32_t>(static_cast<uint32_t>(snap_y) << 9);
            if (vy < 0) vy = -vy;
            continue;
        }
        if (sb.ybottom + snap_y < o_top) {
            py = static_cast<int32_t>(static_cast<uint32_t>(snap_y) << 9);
            if (vy > 0) vy = -vy;
            continue;
        }

        // --- side on: the velocity response, then the damage ---------------------------------
        // The OTHER car's silhouette normal byte for that sector,
        // clamped away from its nose: [0x10, 0xf0].
        int32_t nb = ob.sector_normal[static_cast<size_t>(os)];
        if (nb < 0x10) nb = 0x10;
        if (nb > 0xf0) nb = 0xf0;
        vehicle_hit = j;
        if (last_contact_car != j) vehicle_hit_sound = j;
        last_contact_car = j;                        // a car, this time
        scenery_hit = true;                          // the camera shake
        // ⚠️ `yaw - byte * 256`: the `.FT` stores the normal NEGATED relative to the heading
        // convention. N0 is the outward normal of the other car at the contact, N the way
        // into it. This is the reading that settles the HAYBALE question in physique-decor.md.
        const int32_t n0 = ((o->yaw >> 8) - (nb << 8)) & 0xFFFF;
        const int32_t n = (n0 - 0x8000) & 0xFFFF;
        const int32_t a_self = (n - travel) & 0xFFFF;
        const int32_t a_other = (n - o->travel) & 0xFFFF;
        const int32_t s_sin = fx::mul15(speed, fx::sin16(a_self));
        const int32_t s_cos = fx::mul15(speed, fx::cos16(a_self));
        const int32_t o_cos = fx::mul15(o->speed, fx::cos16(a_other));
        const int32_t o_sin = fx::mul15(o->speed, fx::sin16(a_other));
        const int32_t closing = s_cos - o_cos;       // along N: how fast this car closes in
        // The original's mode 0x20 (see `two_body_contacts`): the other car is a body too.
        VehicleSim* other = (two_body_contacts && !o->replayed) ? field[j] : nullptr;
        if (closing >= 0 && other) {
            // The original's two-body branch of the response, which runs BEFORE this car's own half and
            // from the same components: the other car's normal component gains the same 3/2
            // of the closing speed, pushing it on along N, away from this one.
            const int32_t k = (closing * 3) / 2;
            const int32_t ahead = o_cos + k;
            other->travel = (n - fx::atan2_bam16(o_sin, ahead)) & 0xFFFF;
            other->speed = fx::hypot32(ahead, o_sin);
            // ⚠️ +o_sin, where this car below takes -o_sin: the original's own signs, one for
            // each half.
            other->yaw_rate = o_sin / 400;
        }
        if (closing >= 0) {
            // A restitution of one half: 3/2 of the closing speed comes off the component
            // along N, the tangential one is kept. Truncated toward zero, as the original's
            // signed shift does.
            const int32_t k = (closing * 3) / 2;
            const int32_t back = k - s_cos;          // the new component along N0, outward
            impact_magnitude = closing;              // what the damage reads
            travel = (n0 + fx::atan2_bam16(s_sin, back)) & 0xFFFF;
            speed = fx::hypot32(back, s_sin);
            // The spin: the OTHER car's tangential speed, over 400. Small, and the original's.
            yaw_rate = -o_sin / 400;
            spark_x = x + fx::mul15((sb.zmax - sb.zmin) / 2, fx::sin16(bearing));
            spark_z = z + fx::mul15((sb.zmax - sb.zmin) / 2, fx::cos16(bearing));
            spark_y = y;
            spark_pending = true;
        }
        if (other) {
            // The original's push-out twice: this car +1, the other -1, each HALF of `depth + 4` along
            // N0 - the separation is shared instead of this car taking all of it. Both take
            // the damage of the same impact; a contact already opening (closing < 0) leaves
            // the original's global impact magnitude stale, which here is simply no damage.
            const int32_t half = (depth + 4) / 2;
            const int32_t sx = fx::mul15(half, fx::sin16(n0)) << 9;
            const int32_t sz = fx::mul15(half, fx::cos16(n0)) << 9;
            px += sx;
            pz += sz;
            other->px -= sx;
            other->pz -= sz;
            const int32_t hit = closing >= 0 ? closing : 0;
            take_impact_damage(hit);
            other->take_impact_damage(hit);
            continue;
        }
        push_and_damage(n0, depth);
    }
}

bool VehicleSim::blocks_spot(const VehicleTuning& t, int32_t x, int32_t z, int32_t yaw_bam16) const {
    if (!active || !tune) return false;
    const int32_t dx = (px >> 9) - x, dz = (pz >> 9) - z;
    if (dx > 0x1000 || dx < -0x1000 || dz > 0x1000 || dz < -0x1000) return false;
    const int64_t d2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz;
    const int64_t rr = static_cast<int64_t>(t.box.radius) + tune->box.radius;
    if (d2 > rr * rr) return false;
    const int32_t dist = fx::hypot32(dx, dz);
    const int32_t bearing = fx::atan2_bam16(dx, dz) & 0xFFFF;   // spot -> this car
    return silhouette_contact(t.box.sector_reach.data(), yaw_bam16 & 0xFFFF,
                              tune->box.sector_reach.data(), yaw >> 8, bearing, dist)
        .hit;
}

// The eight box corners at the CURRENT pose, world units - the rotation sample_ground uses.
void VehicleSim::body_corners(int32_t* xs, int32_t* ys, int32_t* zs) const {
    const int32_t a = pitch >> 8, b = yaw >> 8, c = roll >> 8;
    const int32_t sa = fx::sin16(a), ca = fx::cos16(a);
    const int32_t sb = fx::sin16(b), cb = fx::cos16(b);
    const int32_t sc = fx::sin16(c), cc = fx::cos16(c);
    const int32_t m[9] = {
        fx::mul15(fx::mul15(sb, sc), sa) + fx::mul15(cb, cc),
        fx::mul15(cb, sc) - fx::mul15(fx::mul15(sb, cc), sa),
        fx::mul15(ca, sb),
        -fx::mul15(ca, sc),
        fx::mul15(ca, cc),
        sa,
        fx::mul15(fx::mul15(cb, sc), sa) - fx::mul15(sb, cc),
        -(fx::mul15(fx::mul15(cb, cc), sa) + fx::mul15(sb, sc)),
        fx::mul15(ca, cb),
    };
    for (int i = 0; i < 8; ++i) {
        const int32_t lx = tune->box.corner[i][0];
        const int32_t ly = tune->box.corner[i][1];
        const int32_t lz = tune->box.corner[i][2];
        xs[i] = ((px + (((m[0] * lx + m[1] * ly + m[2] * lz) / 32768) << 9)) >> 9);
        ys[i] = ((py + (((m[3] * lx + m[4] * ly + m[5] * lz) / 32768) << 9)) >> 9);
        zs[i] = ((pz + (((m[6] * lx + m[7] * ly + m[8] * lz) / 32768) << 9)) >> 9);
    }
}

// Collision categories 0x12 and 0x1a - a BRIDGE DECK, RIG1's WOODBRIJ. The original's reading,
// with the profile query's three outputs (overshoot, the 0x28/0x2a surface - the deck's UNDERSIDE -
// and the 0x2c/0x2e deck, raised by .FT 0x32 past the footprint's end in Z):
//
//   1. no corner reaches the underside              -> the deck is not there: you pass under;
//   2. else, the body CENTRE was on or over the deck -> queued as a drivable floor;
//   3. else, some corner WAS under the underside     -> a CEILING: the body is lowered by the
//      at the start of the tick                         worst overlap, an upward vy reversed;
//   4. else (the whole body was already up there,     -> a WALL: the blocking response.
//      its centre under the deck: its flank)
//
// Treating it as a plain floor (the port until 2026-09-25) made case 3 a floor too: a car in
// the river whose roof brushed the underside had its lower corners under the deck, and the
// drivable probe lifted them straight onto it - the user's "in the water, hit the bridge,
// teleported onto it" (RIG1). This tick's corners come from the integrated pose; the
// start-of-tick ones are the ring the previous sample_ground left, as in hit_tunnel.
void VehicleSim::hit_bridges() {
    bridge_floor_count = 0;
    if (!terrain || !tune) return;
    const int32_t cx = px >> 9, cz = pz >> 9;
    int32_t cur_x[8], cur_y[8], cur_z[8];
    bool have_cur = false;
    for (const SimObject& o : terrain->objects) {
        if (!o.bridge) continue;
        const int32_t dx = cx - o.x, dz = cz - o.z;
        if (dx >= 0x1001 || dx <= -0x1001 || dz >= 0x1001 || dz <= -0x1001) continue;
        const int64_t reach = static_cast<int64_t>(tune->box.radius) + o.radius;
        if (static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz > reach * reach) continue;
        if (!SimTerrain::touches_box(o, cx, cz, yaw >> 8, tune->box)) continue;
        if (!have_cur) {
            body_corners(cur_x, cur_y, cur_z);
            have_cur = true;
        }
        // 1. how far the highest corner rises past the underside (<= 0; 0 = not touching)
        int32_t over = 0;
        for (int i = 0; i < 8; ++i) {
            ProfileAt p;
            if (!profile_at(o, cur_x[i], cur_z[i], p)) continue;
            over = std::min(over, cur_y[i] - p.roof);   // profile_at is world Y already
        }
        if (over == 0) continue;
        // 2. the centre, where it stood at the start of the tick
        {
            ProfileAt p;
            if (profile_at(o, snap_x, snap_z, p)) {
                const int32_t deck = p.deck + (p.past_z ? o.past_z_lift : 0);
                if (snap_y <= deck) {
                    if (bridge_floor_count < 4) bridge_floor[bridge_floor_count++] = &o;
                    continue;
                }
            }
        }
        // 3. was any corner under the underside at the start of the tick?
        bool was_under = false;
        for (int i = 0; i < 8 && !was_under; ++i) {
            ProfileAt p;
            if (!profile_at(o, corner_x[i], corner_z[i], p)) continue;
            if (corner_y[i] - p.roof >= 0) was_under = true;
        }
        if (was_under) {
            py -= over << 9;           // +Y is down: `over` < 0 lowers the body
            if (vy < 0) vy = -vy;
            continue;
        }
        // 4. its flank: a wall, the ordinary blocking response
        const int32_t bearing = fx::atan2_bam16(o.x - cx, o.z - cz) & 0xFFFF;
        int32_t depth = 0, heading = 0, sign = 1;
        scenery_hit = true;
        if (o.use_box && box_sat(o, cx, cz, yaw >> 8, tune->box, depth, heading, sign)) {
            resolve_contact((sign > 0 ? heading : heading + 0x8000) & 0xFFFF, depth, bearing);
        } else {
            const int32_t dist = fx::hypot32(o.x - cx, o.z - cz);
            resolve_contact((bearing + 0x8000) & 0xFFFF,
                            std::max<int32_t>(0, tune->box.radius + o.radius - dist), bearing);
        }
    }
}

// Collision categories 0x13, 0x14, 0x1b and 0x1c - the tunnels, and the one drivable-looking
// family that is not a floor at all. See SimObject::tunnel for why.
//
// The shape contributes a BORE: two side walls and a roof, and nothing else. All three only
// exist while the body is between the profile's two surfaces, which is the tunnel test and
// the gate the whole case sits behind - outside the bore an ITITUNN is not there, which is what
// leaves the hill above it to the terrain.
//
// ⚠️ Runs on the corner ring the PREVIOUS tick left, like the original: it reads the
// vehicle's corner array, and that array is written by the ground sampler, which
// runs after the collision pass. Same one-tick lag, deliberately.
void VehicleSim::hit_tunnel() {
    if (!terrain || !tune) return;
    const int32_t cx = px >> 9, cy = py >> 9, cz = pz >> 9;
    for (const SimObject& o : terrain->objects) {
        if (!o.tunnel) continue;
        const int32_t dx = cx - o.x, dz = cz - o.z;
        if (dx >= 0x1001 || dx <= -0x1001 || dz >= 0x1001 || dz <= -0x1001) continue;
        const int64_t reach = static_cast<int64_t>(tune->box.radius) + o.radius;
        if (static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz > reach * reach) continue;
        // The original's narrow phase, before the case itself: the body must touch the tube's
        // `.FT` box. The circle above is only the broad phase - without this, a car in one of
        // CITY4's two parallel bores was pushed by the OTHER bore's side wall, the whole way
        // across (user, 2026-09-23: "téléporté dans celui de droite").
        if (!SimTerrain::touches_box(o, cx, cz, yaw >> 8, tune->box)) continue;
        // The tunnel test's own gate: the walls and the roof exist only INSIDE the bore. Above it
        // or below it the shape does nothing - which is the whole of why the hill over a tunnel
        // keeps its relief and why the mouth is not plugged.
        {
            ProfileAt p;
            if (tunnel_role(o, cx, cy, cz, p) != TunnelRole::InBore) continue;
        }

        // --- the two side walls -----------------------------------------------------------
        // Every corner's overshoot past the footprint's local-X bounds; the deepest one wins.
        // `flags & 8` removes the +X wall and `flags & 4` the -X wall, which is how a bore open
        // on one side is authored. ⚠️ A corner past the MOUTH in local Z is skipped - that is
        // the original's flag bit 2, and without it the jambs of the doorway close in on a car
        // that is only half inside.
        int32_t worst = 0, wall_normal = 0;
        for (int i = 0; i < 8; ++i) {
            ProfileAt cp;
            if (!profile_at(o, corner_x[i], corner_z[i], cp)) continue;
            if (cp.past_z) continue;
            const int32_t over = cp.overshoot;
            if (over > 0 && (o.flags & 0x08) == 0 && over > worst) {
                worst = over;
                // Local +X is world bearing `yaw + 0x4000` (invert the rotation two functions
                // up: local (1,0) maps to world (cos yaw, -sin yaw), and a bearing t is
                // (sin t, cos t)). The body has gone too far that way, so it is pushed back
                // along local -X.
                wall_normal = (o.yaw_bam16 + 0xC000) & 0xFFFF;
            } else if (over < 0 && (o.flags & 0x04) == 0 && -over > worst) {
                worst = -over;
                wall_normal = (o.yaw_bam16 + 0x4000) & 0xFFFF;
            }
        }
        if (worst != 0) {
            scenery_hit = true;
            resolve_contact(wall_normal, worst, (wall_normal + 0x8000) & 0xFFFF);
        }

        // --- the roof ---------------------------------------------------------------------
        // `flags & 2` removes it. The deepest corner ABOVE the roof (smaller Y) pushes the
        // whole body back down by that much, and a climbing `vy` is halved and reversed. This
        // is a direct positional clamp in the original too - no reflection, no damage.
        if ((o.flags & 0x02) != 0) continue;
        int32_t worst_up = 0;
        for (int i = 0; i < 8; ++i) {
            ProfileAt cp;
            if (!profile_at(o, corner_x[i], corner_z[i], cp)) continue;
            if (cp.past_z) continue;               // past the mouth: no ceiling out there
            const int32_t gap = corner_y[i] - cp.roof;  // negative: this corner is through it
            if (gap < worst_up) worst_up = gap;
        }
        if (worst_up < 0) {
            py -= worst_up << 9;                   // +Y is down, so this moves the body DOWN
            if (vy < 0) vy = -vy / 2;
        }
    }
}

void VehicleSim::sample_ground() {
    contact_prev = contact;
    near_ground_prev = near_ground;
    contact = 0;
    near_ground = 0;
    push_x = push_y = push_z = 0;
    push_source = 0;
    push_object = nullptr;
    push_worst_y = 0;
    if (!tune || !terrain) return;

    land = terrain->land_type(px, pz);

    // Body rotation, in the game's axes, built the same way the original does - but from the
    // 1024-entry tables, so it stays integer. Row-major Rx(pitch)*Ry(yaw)*Rz(roll).
    const int32_t a = pitch >> 8, b = yaw >> 8, c = roll >> 8;
    const int32_t sa = fx::sin16(a), ca = fx::cos16(a);
    const int32_t sb = fx::sin16(b), cb = fx::cos16(b);
    const int32_t sc = fx::sin16(c), cc = fx::cos16(c);
    const int32_t m[9] = {
        fx::mul15(fx::mul15(sb, sc), sa) + fx::mul15(cb, cc),
        fx::mul15(cb, sc) - fx::mul15(fx::mul15(sb, cc), sa),
        fx::mul15(ca, sb),
        -fx::mul15(ca, sc),
        fx::mul15(ca, cc),
        sa,
        fx::mul15(fx::mul15(cb, sc), sa) - fx::mul15(sb, cc),
        -(fx::mul15(fx::mul15(cb, cc), sa) + fx::mul15(sb, sc)),
        fx::mul15(ca, cb),
    };

    // The candidate gathering, once per tick, from the body centre - then every corner walks that list.
    const ObjectCandidates cand =
        terrain->gather_candidates(px, py, pz, tune->box.radius, &tune->box, yaw >> 8,
                                   bridge_floor, bridge_floor_count);
    in_tunnel = cand.inside;

    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    int32_t max_x = 0, max_y = 0, max_z = 0;
    bool conflict_x = false, conflict_y = false, conflict_z = false;
    int hits = 0;
    const int32_t tol = tune->contact_tolerance();

    for (int i = 0; i < 8; ++i) {
        const int32_t lx = tune->box.corner[i][0];
        const int32_t ly = tune->box.corner[i][1];
        const int32_t lz = tune->box.corner[i][2];
        // Rotate in world units directly: the corner offsets are small (< 300), so a plain
        // 1.15 multiply keeps full precision without any intermediate scaling.
        const int32_t wx = (m[0] * lx + m[1] * ly + m[2] * lz) / 32768;
        const int32_t wy = (m[3] * lx + m[4] * ly + m[5] * lz) / 32768;
        const int32_t wz = (m[6] * lx + m[7] * ly + m[8] * lz) / 32768;
        const int32_t cx_fx = px + (wx << 9);
        const int32_t cy_fx = py + (wy << 9);
        const int32_t cz_fx = pz + (wz << 9);
        // Drivable objects first: standing on a ramp or a bridge REPLACES the terrain under
        // this corner, and forces land type 6 (RAMPS) - the original does exactly this.
        int32_t obj_ground = 0, obj_push[3] = {0, 0, 0};
        const SimObject* who = nullptr;
        const int hit =
            terrain->object_probe(cand, cx_fx, cy_fx, cz_fx, obj_ground, obj_push, &who);
        if (hit != 0) land = 6;
        // The terrain is only consulted when no object claimed this corner - the original skips
        // its terrain call outright on an object hit.
        int32_t t_fx = 0, terr_push[3] = {0, 0, 0};
        if (hit == 0) terrain->terrain_probe(cx_fx, cy_fx, cz_fx, t_fx, terr_push);
        const int32_t g_fx = (hit != 0) ? (obj_ground << 9) : t_fx;

        corner_x[i] = cx_fx >> 9;
        corner_y[i] = cy_fx >> 9;
        corner_z[i] = cz_fx >> 9;
        corner_ground[i] = g_fx >> 9;

        if (hit == -1) {
            const int32_t dx = obj_push[0] << 9, dy = obj_push[1] << 9, dz = obj_push[2] << 9;
            sum_x += dx; sum_y += dy; sum_z += dz;
            auto track_o = [](int32_t v, int32_t& best, bool& conflict) {
                if (best != 0 && ((v ^ best) < 0)) conflict = true;
                if ((best >= 0 && best <= v) || (best <= 0 && v <= best)) best = v;
            };
            track_o(dx, max_x, conflict_x);
            track_o(dy, max_y, conflict_y);
            track_o(dz, max_z, conflict_z);
            if (obj_push[1] > push_worst_y) {
                push_worst_y = obj_push[1];
                push_source = (who && who->platform) ? 3 : 2;
                push_object = who;
            }
            ++hits;
            contact |= static_cast<uint16_t>(1 << i);
            near_ground |= static_cast<uint16_t>(1 << i);
        } else if (hit == 0 && cy_fx > g_fx) {
            // Under the terrain: the terrain push-back, which slides the point along the slope before
            // measuring what penetration is left. See SimTerrain::terrain_probe.
            const int32_t dx = terr_push[0], dy = terr_push[1], dz = terr_push[2];

            sum_x += dx; sum_y += dy; sum_z += dz;
            auto track = [](int32_t v, int32_t& best, bool& conflict) {
                if (best != 0 && ((v ^ best) < 0)) conflict = true;
                if ((best >= 0 && best <= v) || (best <= 0 && v <= best)) best = v;
            };
            track(dx, max_x, conflict_x);
            track(dy, max_y, conflict_y);
            track(dz, max_z, conflict_z);
            if ((dy >> 9) > push_worst_y) {
                push_worst_y = dy >> 9;
                push_source = 1;
                push_object = nullptr;
            }
            ++hits;
            contact |= static_cast<uint16_t>(1 << i);
            near_ground |= static_cast<uint16_t>(1 << i);
        }
        // The wider test: within the vehicle's own contact tolerance of the ground.
        if (corner_ground[i] <= corner_y[i] + tol) near_ground |= static_cast<uint16_t>(1 << i);
    }

    if (hits > 0) {
        push_x = conflict_x ? sum_x / hits : max_x;
        push_y = conflict_y ? sum_y / hits : max_y;
        push_z = conflict_z ? sum_z / hits : max_z;
    }

    // Terrain slope under the body, from the four BOTTOM corners. The corner ring puts 1,2 on
    // zmax and 0,3 on zmin, 2,3 on xmax and 0,1 on xmin - which is what makes these two lines
    // pitch and roll respectively.
    const int32_t depth = tune->box.zmax - tune->box.zmin;
    const int32_t width = tune->box.xmax - tune->box.xmin;
    terrain_pitch = fx::atan2_bam16(
        (corner_ground[1] + corner_ground[2]) - (corner_ground[0] + corner_ground[3]), depth * 2);
    terrain_roll = fx::atan2_bam16(
        (corner_ground[2] + corner_ground[3]) - (corner_ground[0] + corner_ground[1]), width * 2);
    // Keep them signed around zero rather than 0..65535: every consumer compares them to a
    // signed limit.
    auto signed16 = [](int32_t a) { return (a & 0xFFFF) > 0x7FFF ? (a & 0xFFFF) - 0x10000 : (a & 0xFFFF); };
    terrain_pitch = signed16(terrain_pitch);
    terrain_roll = signed16(terrain_roll);

    if (near_ground == 0) {
        int32_t h = 0;
        for (int i = 0; i < 4; ++i) h += corner_ground[i] - corner_y[i];
        height_above = h / 4;
        land = 7; // AIR
    }
}

void VehicleSim::apply_impact() {
    // The original's impact response. Runs on the tick's resolved push-back, BEFORE the engine,
    // and it is what stops a vehicle accumulating vertical velocity for ever while it sits on the
    // ground.
    //
    // ⚠️ All four `bounce` values are live. An earlier, too hasty reading concluded that
    // entries 0 and 2 were dead; a closer look at the original says
    // otherwise, and the vehicle fell through its own floor until this existed.
    if (contact == 0) return;
    const int32_t* B = tune->bounce[land & 7];
    const int32_t b0 = static_cast<int16_t>(B[0]);
    const int32_t b1 = static_cast<int16_t>(B[1]);
    const int32_t b2 = static_cast<int16_t>(B[2]);

    if (push_x == 0 && push_z == 0) {
        // Flat ground: a plain vertical rebound, scaled by bounce[0].
        if ((vy ^ push_y) < 0) return; // already moving away from the surface
        vy = -fx::mul15(vy, b0);
        return;
    }

    // Oblique: reflect the velocity about the push direction, removing (1 + bounce) of its
    // normal component - bounce[1] vertically, bounce[2] horizontally.
    const int32_t len = fx::hypot32(fx::hypot32(push_x, push_y), push_z);
    if (len == 0) return;
    const int32_t nx = fx::muldiv(push_x, 0x7FFF, len);
    const int32_t ny = fx::muldiv(push_y, 0x7FFF, len);
    const int32_t nz = fx::muldiv(push_z, 0x7FFF, len);

    int32_t vx = fx::mul15(speed, fx::sin16(travel));
    int32_t vz = fx::mul15(speed, fx::cos16(travel));
    int32_t vyv = vy;
    const int32_t d = fx::mul15(vx, nx) + fx::mul15(vyv, ny) + fx::mul15(vz, nz);
    if (d <= 0) return;

    const int32_t pxc = fx::mul15(d, nx);
    const int32_t pyc = fx::mul15(d, ny);
    const int32_t pzc = fx::mul15(d, nz);
    vx -= pxc + fx::mul15(pxc, b2);
    vyv -= pyc + fx::mul15(pyc, b1);
    vz -= pzc + fx::mul15(pzc, b2);

    vy = vyv;
    travel = fx::atan2_bam16(vx, vz) & 0xFFFF; // X is the sine axis, Z the cosine axis
    speed = fx::hypot32(vx, vz);
}

void VehicleSim::read_input(const VehicleInput& in) {
    // Steering, then the throttle channel the vehicle's `acceling` selects.
    ramp_axis(steer_axis, in.right, in.left, kRampSteer, kFullScale);

    // The original's speed-sensitive steering curve.
    {
        const int32_t* S = tune->steering;
        const bool backwards = vfwd < 0;
        const int32_t v = backwards ? -vfwd : vfwd;
        int32_t r = 0;
        if (v >= S[3]) {
            r = S[4];
        } else if (v >= S[1]) {
            r = S[2] - fx::muldiv(v - S[1], S[2] - S[4], S[3] - S[1]);
        } else if (S[1] != 0) {
            r = fx::muldiv(S[2], v, S[1]);
        }
        steer_target = steer_axis * r;
        if (S[0] != 0 && backwards) steer_target = -steer_target;
    }

    const int32_t mode_sel = tune->acceling[1];
    if (mode_sel == 0) {
        ramp_axis(throttle_axis, in.accel, in.brake, kRampSteer, kFullScale);
        if (throttle_axis < 0) { throttle_fwd = 0; throttle_rev = -throttle_axis; }
        else { throttle_rev = 0; throttle_fwd = throttle_axis; }
    } else if (mode_sel == 1) {
        ramp_single(throttle_fwd, in.accel, kRampThrottle, kFullScale);
        ramp_single(throttle_rev, in.brake, kRampThrottle, kFullScale);
    } else {
        ramp_axis(throttle_fwd, in.accel, in.brake, kRampThrottle, kFullScale);
        throttle_rev = 0;
        if (throttle_fwd < 0) throttle_fwd = 0;
    }

    // Which way the pedals are ALLOWED to push, given which way we are already going.
    if (in.accel) may_accel = vfwd > -kDirGuard;
    if (in.brake) may_reverse = vfwd < kDirGuard;

    input_bits = 0;
    if (in.accel) input_bits |= may_accel ? 0x1 : 0x4;
    if (in.brake) input_bits |= may_reverse ? 0x2 : 0x4;

    // While a boost runs, the game holds the accelerator down for you and ignores the brake.
    if (boost_timer > 0) {
        input_bits = 0x9;
        may_accel = true;
        may_reverse = false;
        --boost_timer;
    }
}

void VehicleSim::update_nitro(const VehicleInput& in) {
    const int32_t* N = tune->nitro;
    if (N[0] == 3) {
        // Aircraft: `nitro` is the collective, not a boost. The game also flies it for you when
        // you get too low - that is not an assist bolted on, it is in the original's nitro handling.
        const bool both = in.accel && in.brake;
        bool auto_lift = false;
        if (!both && height_above < 250 && vy > -700) auto_lift = true;
        ramp_single(lift_axis, in.nitro || auto_lift, kRampLift, kFullScale);

        const int32_t span = N[4] - N[3];
        int32_t v = N[4] + vy;
        if (v > 0) v = (v > span) ? N[2] : fx::muldiv(v, N[2], span);
        else v = 0;
        vy -= v * lift_axis / 256;
        if (!both) { if (vy > N[1]) vy = N[1]; }
        else vy = v + tune->gravity() * 3;
        boost_active = lift_axis > 64;
        return;
    }

    // Ground vehicles: a timed boost off a limited reserve. Mode 1 fires on the key's EDGE,
    // mode 2 while it is held.
    const bool fire = in.nitro && (N[0] == 2 || !prev_nitro_key);
    nitro_refused = false;
    if (fire && nitro_fuel > 0 && boost_timer <= 0) {
        --nitro_fuel;
        boost_timer = N[1];
    } else if (fire && nitro_fuel < 1) {
        // The empty-tank refusal, the original's own branch. Audio only.
        nitro_refused = true;
    }
    prev_nitro_key = in.nitro;

    boost_active = boost_timer > 0;
    if (boost_active) {
        flame_phase = static_cast<uint8_t>(flame_phase + 1);
        if (flame_phase > 0x20) flame_phase = 0x11;
    } else {
        flame_phase = 0;
    }
}

void VehicleSim::run_engine() {
    // The original's engine.
    //
    // THE SPEED CLASS (RaceRules::speed_percent, ours): the gear table is read at the speed
    // brought back to 100 % and its thrust scaled up, i.e. the curve is stretched along BOTH
    // axes - the top speed and the acceleration grow together, and the gear and the rev needle
    // (the engine's pitch) stay in the table's own range. The nitro's cap and push, and the two
    // slowing rates below, follow, so every step is the original's times the class. At 100 the
    // arithmetic is skipped, not just neutral: the reference race must stay bit for bit.
    const int32_t pct = race_rules().speed_percent;
    const bool scaled = pct != 100;
    const auto at_class = [&](int32_t v) { return scaled ? fx::muldiv(v, 100, pct) : v; };
    const auto by_class = [&](int32_t v) { return scaled ? fx::muldiv(v, pct, 100) : v; };
    const auto pair_by_class = [&](const SkidPair& p) {
        return SkidPair{by_class(p.low), by_class(p.high)};
    };
    thrust = 0;
    if (input_bits & 0x1) {
        thrust = by_class(gear_curve(at_class(vfwd), tune->gear_f)) * throttle_fwd / 256;
    } else if ((input_bits & 0x3) == 0x2) {
        thrust = -by_class(gear_curve(at_class(-vfwd), tune->gear_b)) * throttle_rev / 256;
    } else {
        gear_curve(at_class(vfwd >= 0 ? vfwd : -vfwd), vfwd >= 0 ? tune->gear_f : tune->gear_b);
    }

    if (input_bits & 0x8) {
        const int32_t* N = tune->nitro;
        const int32_t cap = by_class(N[4]);
        const int32_t span = cap - by_class(N[3]);
        const int32_t push = by_class(N[2]);
        const int32_t d = cap - vfwd;
        int32_t boost;
        if (d <= 0) boost = d;                 // past the cap: this pulls BACK
        else if (d > span) boost = push;
        else boost = fx::muldiv(d, push, span);
        thrust += boost;
    }

    if (thrust != 0) {
        // Uphill costs thrust, and past `bounce[3]` there is none left at all.
        const int32_t s = (thrust > 0) ? -terrain_pitch : terrain_pitch;
        const int32_t limit = tune->bounce[land & 7][3];
        if (limit >= 0) {
            if (s >= limit) thrust = 0;
            else if (s > 0) thrust = fx::muldiv(thrust, limit - s, limit);
        }
    }

    // ⚠️ THE TAIL OF THE ORIGINAL'S ENGINE, and it is not optional: this is the ONLY thing that slows a
    // vehicle down. `fvel` is 0 on almost every surface of almost every script, so without these
    // two a car coasts for ever and the brake does nothing at all.
    //
    //   no throttle, no brake -> `fdam` drags the forward speed toward 0 (engine braking)
    //   bit 2 set (braking)   -> `brk` does, and its result feeds the `mode` flag
    //
    // Both target ZERO - they are rate limiters on the way down, not forces.
    auto apply_slowing = [&]() {
        if ((input_bits & 0x4) != 0) {
            mode_accum |= approach_mode(vfwd, 0, pair_by_class(tune->brk[land & 7]));
        } else if ((input_bits & 0x3) == 0) {
            // Result deliberately NOT OR-ed into `mode` - see the spec's list.
            int32_t v = vfwd;
            approach_mode(v, 0, pair_by_class(tune->fdam[land & 7]));
            vfwd = v;
        }
    };

    if (thrust == 0) {
        apply_slowing();
        return;
    }
    switch (tune->acceling[0]) {
    case 0:
        vfwd += thrust; // aircraft: straight down the body axis, no slope decomposition
        break;
    case 2: {
        const int32_t ang = pitch >> 8;
        vfwd += fx::mul15(thrust, fx::cos16(ang));
        vy += fx::mul15(thrust, fx::sin16(ang));
        break;
    }
    case 1:
    default: {
        vfwd += fx::mul15(thrust, fx::cos16(terrain_pitch));
        vy += fx::mul15(thrust, fx::sin16(terrain_pitch));
        break;
    }
    }
    apply_slowing();
}

void VehicleSim::apply_grip() {
    const uint16_t grip_mask = static_cast<uint16_t>(tune->wheels[1]);
    if ((near_ground & grip_mask) != 0 || grip_mask == 0) {
        if (tune->is_aircraft() && near_ground != 0) {
            mode_accum |= approach_mode(yaw_rate, 0, tune->yavel[land & 7]);
        } else {
            mode_accum |= approach_mode(yaw_rate, steer_target, tune->yavel[land & 7]);
            if (near_ground != 0) {
                // `rpush`, as in the original. A cambered surface steers you down its slope.
                const int32_t* R = tune->rpush;
                int32_t d = terrain_roll;
                const bool inside = (d >= 0) ? (d >= R[2]) : (-d >= R[2]);
                if (inside && R[0] != 0 && R[1] != 0 && R[3] != R[2]) {
                    if (d > R[3]) d = R[3];
                    if (d < -R[3]) d = -R[3];
                    int32_t v = (vfwd < 0) ? -vfwd : vfwd;
                    if (v > R[1]) v = R[1];
                    int32_t t = fx::muldiv(fx::muldiv(R[0], v, R[1]), d, R[3] - R[2]);
                    if (vfwd < 0) t = -t;
                    const int32_t stepv = t / 4;
                    if (t >= 0) { if (t > yaw_rate) yaw_rate += stepv; }
                    else { if (t < yaw_rate) yaw_rate += stepv; }
                }
            }
        }
    }

    // Turning can scrub speed - only if the vehicle's own steering[5] says so.
    if (steer_target != 0 && tune->steering[5] != 0) {
        int32_t v = tune->steering[5] * steer_axis / 256;
        if (v < 0) v = -v;
        approach(vfwd, 0, v);
    }

    // THE SKID FLAG, read HERE and not later: the original tests the lateral velocity
    // on the line above the `svel` decay, so it sees this tick's slip before it is bled off.
    // Threshold and placement are the original's - see the declaration in vehicle.h.
    if (vlat < -5000 || vlat > 5000) skid_accum = true;

    // THE line that makes a car turn: the lateral component decays toward zero, and the travel
    // direction is rebuilt from what is left.
    mode_accum |= approach_mode(vlat, 0, tune->svel[land & 7]);

    if (near_ground != 0) {
        // Rolling resistance. Its result is deliberately NOT fed into `mode` - see the spec.
        int32_t dummy = vfwd;
        approach_mode(dummy, 0, tune->fvel[land & 7]);
        vfwd = dummy;
    }
}

void VehicleSim::apply_air() {
    // Not driving: the yaw rate decays and both components bleed off.
    if (contact != 0) {
        mode_accum |= approach_mode(yaw_rate, 0, tune->iavel[land & 7]);
        approach(vfwd, 0, tune->svel[land & 7].low);
        mode_accum |= approach_mode(vlat, 0, tune->svel[land & 7]);
        // Inside the contact test, like the rest: wholly airborne, the original leaves the
        // gear table alone and the revs climb 5 a tick instead (the tail of step()).
        gear_curve(vfwd >= 0 ? vfwd : -vfwd, vfwd >= 0 ? tune->gear_f : tune->gear_b);
    }
    thrust = 0;
}

void VehicleSim::apply_tilter() {
    // The original's two tilter routines and their fallbacks. This writes the REAL angular rates, so it
    // is simulation, not decoration - see the spec.
    auto default_roll = [&]() {
        if (roll_rate == 0) return;
        const uint16_t same = (roll_rate < 0) ? kCornerMaskRollB : kCornerMaskRollA;
        if ((near_ground & same) != 0) roll_rate /= 2;
        if ((contact & same) == 0) return;
        if ((contact_prev & same) == 0) roll_rate = -roll_rate; // fresh impact: bounce
    };
    auto default_pitch = [&]() {
        if (pitch_rate == 0) return;
        const uint16_t same = (pitch_rate < 0) ? kCornerMaskPitchB : kCornerMaskPitchA;
        if ((near_ground & same) != 0) pitch_rate /= 2;
        if ((contact & same) == 0) return;
        if ((contact_prev & same) == 0) pitch_rate = -pitch_rate;
    };

    bool handled = false;
    const TilterAxis& tz = tune->tilter_z;
    if (tz.mode == 1) {
        // Lean out of the corner, up to yaw_rate * a past the terrain's own camber.
        if ((near_ground & 0x9) != 0 && vfwd >= 0 && !rolled_over) {
            const int32_t base = -(terrain_roll * 256);
            // ⚠️ NOT THE ORIGINAL AT A YAW RATE OF EXACTLY 0. The original splits on
            // `yaw_rate < 0`, so 0 - driving straight, where `yavel` lands the
            // rate on 0 exactly - takes the lean branch too. Ported that way, a JEEP (b = 32768,
            // 0.7 degree a tick) standing still leans 0.35 to 0.7 degree to one side for good
            // and never settles (bench TILT/TORQUE, 2026-09-24): the very "slightly on two
            // wheels" the user reported as a defect. Left out pending the user's verdict.
            if (yaw_rate < 0) {
                if (base - yaw_rate * tz.a >= roll) {
                    if (roll_rate < 0) roll_rate = 0;
                    roll_rate += tz.b;
                    snap_roll = snap_pitch = false; // the lean takes over from the snap
                    handled = true;
                }
            } else if (yaw_rate > 0) {
                if (roll >= base - yaw_rate * tz.a) {
                    if (roll_rate > 0) roll_rate = 0;
                    roll_rate -= tz.b;
                    snap_roll = snap_pitch = false;
                    handled = true;
                }
            }
        }
    } else if (tz.mode == 3 && contact == 0) {
        const int32_t target = speed * tz.a;
        int32_t d = roll - target;
        if (d >= 0) roll = (d > tz.c) ? (roll - tz.b) : target + fx::muldiv(tz.c - tz.b, d, tz.c);
        else roll = (d < -tz.c) ? (roll + tz.b) : target + fx::muldiv(tz.c - tz.b, d, tz.c);
        handled = true;
    }
    if (!handled) default_roll();

    handled = false;
    tilter_x_fired = false;
    const TilterAxis& tx = tune->tilter_x;
    if (tx.mode == 1) {
        // Squat and dive: the body pitches with this tick's actual acceleration.
        if ((near_ground & 0x9) != 0 && vfwd >= 0 && accel_delta > 0) {
            if (pitch >= (terrain_pitch * 256) - accel_delta * tx.a) {
                if (pitch_rate > 0) pitch_rate = 0;
                pitch_rate -= tx.b;
                snap_roll = snap_pitch = false;
                handled = true;
                tilter_x_fired = true;
            }
        }
    } else if (tx.mode == 3 && contact == 0) {
        const int32_t target = vfwd * tx.a;
        int32_t d = pitch - target;
        pitch_rate = 0;
        if (d >= 0) pitch = (d > tx.c) ? (pitch - tx.b) : target + fx::muldiv(tx.c - tx.b, d, tx.c);
        else pitch = (d < -tx.c) ? (pitch + tx.b) : target + fx::muldiv(tx.c - tx.b, d, tx.c);
        handled = true;
    }
    if (!handled) default_pitch();
}

void VehicleSim::normalise_angles() {
    // As in the original: past +/-90 degrees of pitch the body is folded over and marked upside down.
    // The roll test comes first, on the roll as it is before the fold.
    rolled_over = ((roll + 0x400000) & 0x800000) != 0;
    if (((pitch + 0x400000) & 0x800000) != 0) {
        pitch = 0x800000 - pitch;
        yaw += 0x800000;
        roll += 0x800000;
        pitch_rate = -pitch_rate;
        upside_down = !upside_down;
    }
}

void VehicleSim::dress_replayed() {
    if (!tune || !terrain) return;
    ++tick;
    // The corners, the surface and the near-ground mask, exactly as a driven car samples them.
    // The push-back it computes is ignored: a ghost goes where its tape says.
    sample_ground();
    // The body-frame velocity, the inverse of recompose(): travel - yaw is atan2(vlat, vfwd).
    const int32_t rel = (travel - (yaw >> 8)) & 0xFFFF;
    vfwd = static_cast<int32_t>((static_cast<int64_t>(speed) * fx::cos16(rel)) >> 15);
    vlat = static_cast<int32_t>((static_cast<int64_t>(speed) * fx::sin16(rel)) >> 15);
    // VehicleSim::skidding's own rule: a sideways slide over 5000, on the ground.
    skidding = near_ground != 0 && (vlat > 5000 || vlat < -5000);
    moving = vfwd > 3000 || vfwd < -3000;
    if (tune->wheel_radius > 0)
        wheel_spin = (wheel_spin + fx::muldiv(vfwd, 65536, 3217 * tune->wheel_radius)) & 0xFFFF;
    rotor_spin = (rotor_spin + kRotorSpinPerTick) & 0xFFFF;
}

void VehicleSim::recompose() {
    // The speed class stretches the two clamps with the engine (run_engine): without it every
    // class past 150CC stopped at the same 0xB4FA.
    const int32_t pct = race_rules().speed_percent;
    const int32_t comp = pct == 100 ? kCompClamp : fx::muldiv(kCompClamp, pct, 100);
    const int32_t most = pct == 100 ? kSpeedClamp : fx::muldiv(kSpeedClamp, pct, 100);
    vfwd = clamp32(vfwd, -comp, comp);
    vlat = clamp32(vlat, -comp, comp);
    travel = (fx::atan2_bam16(vlat, vfwd) + (yaw >> 8)) & 0xFFFF;
    speed = clamp32(fx::hypot32(vfwd, vlat), -most, most);
}

void VehicleSim::step(const VehicleInput& orders) {
    if (!tune || !terrain) return;
    ++tick;
    mode_accum = false;
    skid_accum = false;
    thrust = 0;

    snap_y = py >> 9;   // the original's snapshot, taken before anything moves
    snap_x = px >> 9;
    snap_z = pz >> 9;
    integrate();
    if (py < kCeilingY) py = kCeilingY;
    vy += tune->gravity();
    // Scenery collision runs BETWEEN the integration and the ground sampling, which is where
    // the original puts it (the collision pass, then the ground sampler). One consequence is
    // deliberate: the ground height the vertical gate reads is LAST tick's, because this tick's
    // is not computed yet. The original has the same property - it still holds the previous value.
    hit_scenery();
    hit_tunnel();      // same pass, same corner ring: it is one `switch` in the original
    hit_bridges();     // ...and so is the bridge case
    hit_vehicles();    // and then the other racers, still inside the same pass
    head_kick = scenery_hit ? (vfwd >> 4) : 0;   // the pass's tail - see the declaration
    sample_ground();
    // Captured BEFORE apply_impact(), which is what turns the fall into a bounce: one tick later
    // `vy` no longer holds the speed the ground was struck at. See VehicleSim::landing_impact.
    landing_impact = (near_ground_prev == 0 && near_ground != 0 && vy > 0) ? vy : 0;
    apply_impact();

    // A WRECK TAKES NO ORDERS. The original zeroes all six controls when the damage is negative
    // - steering, throttle, brake, boost and the recovery key alike - and it does
    // so AFTER the collision pass, so the impact that destroys the car already counts. The car
    // coasts to a stop where it died, and ALT cannot bring it back: a wreck stays a wreck until
    // the race is over.
    const VehicleInput in = damage < 0 ? VehicleInput() : orders;
    wrecked_ticks = damage < 0 ? wrecked_ticks + 1 : 0;
    read_input(in);
    update_nitro(in);
    if (boost_timer > 0) input_bits |= 0x8;

    // Decompose the polar velocity into the body frame, using the yaw the integration just left.
    {
        const int32_t slip = (travel - (yaw >> 8)) & 0xFFFF;
        vfwd = fx::mul15(speed, fx::cos16(slip));
        vlat = fx::mul15(speed, fx::sin16(slip));
    }
    const int32_t vfwd_before = vfwd;

    const uint16_t drive_mask = static_cast<uint16_t>(tune->wheels[0]);
    // ...and it does not drive: the original sends a wreck down the same branch as a car in the
    // air, where nothing pushes it.
    const bool driving = enabled && damage >= 0 && ((near_ground & drive_mask) != 0 || drive_mask == 0);
    if (driving) {
        run_engine();
        apply_grip();
    } else {
        apply_air();
    }

    if (near_ground != 0) {
        px -= push_x;
        py -= push_y;
        pz -= push_z;

        // As in the original: "is the body already settled onto the terrain's own angle?", asked of
        // the rates AS THEY ARE NOW.
        //
        // ⚠️ THE ORDER IS THE WHOLE POINT. This must be decided BEFORE the righting torque
        // below, because that torque (36863 for a JEEP) is three times the 0x3000 threshold: ask
        // afterwards and the answer is always no, the rate is never zeroed, and it accumulates
        // until the vehicle somersaults. That was exactly the "it pitches forward far too fast,
        // the boat flips on itself right off the WATER1 ramp" defect reported 2026-09-15.
        snap_pitch = false;
        snap_roll = false;
        if (pitch_rate > -0x3000 && pitch_rate < 0x3000) {
            const int32_t t = terrain_pitch * 256;
            if (pitch > t - 0x20000 && pitch < t + 0x20000) snap_pitch = true;
        }
        if (roll_rate > -0x3000 && roll_rate < 0x3000) {
            const int32_t t = terrain_roll * 256;
            if (-roll > t - 0x20000 && -roll < t + 0x20000) snap_roll = true;
        }

        // The original's righting torque. Only one side supported: the body is rotated toward the ground on that
        // axis. When the snap above fires, it cancels this again - that is the design.
        //
        // The torque is added RAW, every tick, exactly as the original does it. It is what
        // makes a landing SNAP. The brake on it is the chain that follows - the original's
        // half-damping once the leading corner group comes near the ground, then the 0x3000
        // snap window - and that chain only works on the ground, which is the point: this is a
        // contact force, not angular momentum.
        //
        // ⚠️ FORMER DELIBERATE DEVIATION 2 - RETIRED 2026-09-27, kept switchable
        // (RaceRules::righting_tolerance, off by default). Measured against the pitch the
        // original's ghosts recorded over their jumps (physics_bench SAUT), it piled up pitch rate
        // on every tick before a take-off, and deviation 3's tight cap was only hiding that; with
        // it gone and the cap loosened to 0x10000 the jumps match the original. The price is the
        // slower landing recovery described below, which the user tried and kept. What follows is
        // why it was put in, in the first place.
        //
        // The group test read `near_ground`, where
        // the original reads `contact` (PENETRATION only). Measured reason, from the
        // bench's LANDING trace: a body that comes down nose-first POGOS. The first impact
        // rebounds it 3 units off its own nose, gravity takes four ticks to bring it back, and
        // `contact` is therefore set on roughly one tick in five - so the torque fires four
        // times over a 23-tick landing and the body creeps back to level. Reading the tolerance
        // mask instead fires it on every tick the nose is actually down, which is the same
        // thing physically: `cogs[2]`, the tolerance, is 10 units, and it exists to say "this
        // corner is on the ground". The user asked for this landing to be about twice as fast
        // (2026-09-15); measured, it is.
        //
        // ⚠️ But the tolerance mask ALONE loses the original's own case, so the test takes
        // EITHER mask. A body resting a few degrees off level with a NARROW track has both sides
        // within the 10-unit tolerance - a JEEP is 84 units wide, so at 4 degrees of roll its two
        // sides are 6 units apart - and the tolerance mask then says "both down": no torque. The
        // 0x20000 snap window is only 2.8 degrees, so nothing else rights it either, and the car
        // sat on two wheels for good (the 2026-09-23 report; physics_bench's TILT block). The
        // lower side still PENETRATES every other tick, which is exactly what the original's
        // `contact` test answers. The two masks cannot disagree - a penetrating corner is also
        // within tolerance - so this never pushes both ways in one tick.
        auto one_side = [&](uint16_t a, uint16_t b) {
            return (race_rules().righting_tolerance &&
                    (near_ground & a) != 0 && (near_ground & b) == 0) ||
                   ((contact & a) != 0 && (contact & b) == 0);
        };
        if (one_side(kCornerMaskRollA, kCornerMaskRollB)) roll_rate -= tune->roll_righting();
        if (one_side(kCornerMaskRollB, kCornerMaskRollA)) roll_rate += tune->roll_righting();
        if (one_side(kCornerMaskPitchA, kCornerMaskPitchB)) pitch_rate -= tune->pitch_righting();
        if (one_side(kCornerMaskPitchB, kCornerMaskPitchA)) pitch_rate += tune->pitch_righting();
    }
    if (py < kCeilingY) py = kCeilingY;

    normalise_angles();
    apply_tilter();

    // Settled on the ground: drop the body onto the terrain's angle and kill the rate with it.
    // The flags were decided before the righting torque, and the tilter clears them when it
    // takes over - see apply_tilter.
    if (near_ground != 0) {
        if (snap_pitch) { pitch_rate = 0; pitch = terrain_pitch * 256; }
        if (snap_roll) { roll_rate = 0; roll = -terrain_roll * 256; }
        air_ticks = 0;
    } else {
        ++air_ticks;
        // ⚠️ DELIBERATE DEVIATION 3 of 4. The others are the platform proximity test in
        // object_probe, the righting torque's group test just above, and the terrain push-back
        // in terrain_probe; the spec lists all four in one place.
        //
        // Nothing in the original damps the angular rates in the air - confirmed:
        // its two damping steps halve only while the corner group is near
        // the ground, and `cogs[4]`, the one airborne drift there is, is 0 on most vehicles.
        // So whatever the righting torque piled up over the last few ticks of contact leaves
        // the ground with the body and turns it at that rate for the whole flight. Driving off
        // the end of a ramp, that is 65535 a tick for an INFLATE - 50 degrees a second, held
        // for the entire jump.
        //
        // That is half of the defect the user reported on 2026-09-15: "it turns fast in the
        // air, where it should be slow, and slow on the ground, where it should be fast". So
        // the body's airborne rate is capped at the engine's own airborne limit - see
        // kAirRateCap. On the ground the torque is untouched by this; the moment there is no
        // ground to push against, what is left is bounded.
        //
        // Measured over 12 levels and ~500 jumps taken while driving: no flight turns more
        // than 90 degrees, and the worst turns 54.
        const RaceRules& rr = race_rules();
        if (rr.air_rate_cap) {
            const int32_t cap = rr.air_rate_cap_value;
            const int32_t roll_cap = rr.air_roll_cap_value;
            pitch_rate = clamp32(pitch_rate, -cap, cap);
            roll_rate = clamp32(roll_rate, -roll_cap, roll_cap);
        }
        if (rr.air_damp_shift > 0) pitch_rate -= pitch_rate / (1 << rr.air_damp_shift);
        {
            const int32_t sh = rr.air_roll_damp_shift > 0 ? rr.air_roll_damp_shift : rr.air_damp_shift;
            if (sh > 0) roll_rate -= roll_rate / (1 << sh);
        }
        // Airborne pitch drift, if this vehicle asks for one.
        const int32_t rate = tune->air_pitch_rate(), limit = tune->air_pitch_limit();
        if (rate > 0 && pitch_rate < limit) pitch_rate += rate;
        if (rate < 0 && pitch_rate > limit) pitch_rate += rate;
        if (rev < 250) rev += 5;
    }

    // As in the original, the polar velocity is rebuilt from the two components only on a tick that
    // touched them - the driving branch, or the other one with a corner penetrating. Wholly
    // airborne, `speed` and `travel` are left exactly as they were, and the acceleration the
    // `tilter x` reads next tick is 0.
    const bool recomposed = driving || contact != 0;
    if (recomposed) recompose();
    if (rev < 0) rev = 0;

    // Wheel roll. `vfwd` is in 1/512 world units per tick; a wheel of radius r turns by
    // distance/r radians, i.e. distance * 65536 / (2*pi*r) in BAM16. 2*pi*512 ~= 3217, so
    // BAM16 = vfwd * 65536 / (3217 * r) - all integer, no trig.
    if (tune->wheel_radius > 0) {
        wheel_spin = (wheel_spin + fx::muldiv(vfwd, 65536, 3217 * tune->wheel_radius)) & 0xFFFF;
    }
    // Rotors and props turn at a constant rate, engine-driven and independent of road speed -
    // a helicopter's rotor does not slow down because it is hovering. One eighth of a turn per
    // 28 ms tick, so a full turn in 224 ms. ⚠️ THE RATE IS A HYPOTHESIS, not the original's value;
    // it stays under half a turn per tick so the interpolated display cannot alias backwards.
    rotor_spin = (rotor_spin + kRotorSpinPerTick) & 0xFFFF;
    accel_delta = recomposed ? vfwd - vfwd_before : 0;
    mode = mode_accum;
    // The other two scratch bytes of the tail of the original's vehicle update. `moving` is unconditional - it sits
    // outside the ground/air branch in the original too.
    skidding = skid_accum;
    moving = vfwd > 3000 || vfwd < -3000;

    if (in.recover) recover();
}

} // namespace bigred
