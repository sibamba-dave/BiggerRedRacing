#include "sim/race.h"

#include <algorithm>

#include "sim/fixed.h"

namespace bigred {
namespace {

// The original keeps ONE trig table and reaches into it at two bases 0x200 bytes - 256
// entries, a quarter turn - apart. Every formula here is written in terms of those two bases so
// it stays recognisable next to the original.
//
// ⚠️ WHICH ONE IS THE SINE WAS READ OUT OF THE ORIGINAL GAME, not inferred. The quarter-turn gap
// is symmetric and says nothing on its own: sin(i+256) = cos(i) works either way round. The
// bytes do not - at the first base the table starts 0, 201, 402, 603 (a sine through zero) and
// at the second it starts 32767, 32766, 32764 (a cosine at its peak).
//
// Getting this backwards, which is what the first port did, rotates every gate a quarter turn
// about its own anchor: the ring then lies ACROSS the road instead of along it, checkpoints are
// missed, and a respawn drops the car on the verge.
inline int32_t tbl_sin(int32_t idx) { return fx::kSinTable[idx & 0x3FF]; }          // the sine
inline int32_t tbl_cos(int32_t idx) { return fx::kSinTable[(idx + 256) & 0x3FF]; }  // the cosine

// The original's own integer-division truncation - toward zero, not toward minus infinity.
inline int32_t div_trunc(int64_t num, int32_t den) {
    if (den == 0) return 0;
    return static_cast<int32_t>(num / den);
}

int16_t rd16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
int32_t rd32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

} // namespace

std::vector<Gate> parse_gates(const uint8_t* map_cse, size_t bytes) {
    std::vector<Gate> out;
    if (!map_cse) return out;
    // 16 bytes of file per record. The original copies them into a 17-byte slot and puts its own
    // flag byte AFTER them, set to 0xff at load. Nothing in the file carries that flag; `unsafe`
    // is what clears it.
    const size_t n = bytes / 16;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* r = map_cse + i * 16;
        Gate g;
        g.x = rd32(r);
        g.z = rd32(r + 4);
        g.angle = rd16(r + 8);
        g.length = rd16(r + 10);
        g.half_depth = rd16(r + 12);
        g.half_depth_delta = rd16(r + 14);
        out.push_back(g);
    }
    return out;
}

void mark_unsafe_gates(std::vector<Gate>& gates, const std::vector<int>& unsafe_indices) {
    for (int i : unsafe_indices) {
        if (i >= 0 && i < static_cast<int>(gates.size())) gates[static_cast<size_t>(i)].safe = false;
    }
}

std::vector<CheckLine> build_check_lines(
    const std::vector<SceneryInstance>& instances, const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup) {
    std::vector<CheckLine> out;
    for (const SceneryInstance& in : instances) {
        if (in.shape_index < 0 || static_cast<size_t>(in.shape_index) >= shape_names.size())
            continue;
        // By CATEGORY, the way the original's scenery pass dispatches - not by shape name. Indices 10 and 14
        // are raw 50 and 54, and on the shipped levels only `CHECKP` uses them.
        const int idx = collision_category_index(in.collision_category_byte);
        if (idx != 10 && idx != 14) continue;
        CheckLine c;
        c.x = static_cast<int32_t>(in.x);
        c.y = static_cast<int32_t>(in.y);
        c.z = static_cast<int32_t>(in.z);
        c.yaw_bam16 = in.angle_y & 0xFFFF;
        c.number = in.variant_index;
        c.window = in.aux_byte;
        c.height_gated = (idx == 14);
        // The box, from the shape's own `.FT` when it is the 0xfffd sentinel (it is, on every
        // shipped CHECKP.FT); the defaults above are that file's values otherwise.
        if (const Blob* ft = ft_lookup(shape_names[static_cast<size_t>(in.shape_index)])) {
            auto s16 = [ft](size_t o) {
                return static_cast<int32_t>(static_cast<int16_t>(
                    static_cast<uint16_t>((*ft)[o]) | (static_cast<uint16_t>((*ft)[o + 1]) << 8)));
            };
            if (ft->size() >= 0x20 && (s16(0x16) & 0xFFFF) >= 0xfffd) {
                c.box_xmin = s16(0x18);
                c.box_xmax = s16(0x1a);
                c.box_zmin = s16(0x1c);
                c.box_zmax = s16(0x1e);
                c.y_top = s16(0x06);
            }
        }
        out.push_back(c);
    }
    return out;
}

int gate_of_point(const std::vector<Gate>& gates, int32_t x, int32_t z, bool* inside) {
    int best = -1;
    // ⚠️ NOT "the first gate that contains it". Gates overlap, so a point is often inside two
    // of them, and the lowest-numbered one can begin well upstream. Take the gate whose ENTRY
    // FACE is closest behind the point.
    int32_t best_entry = 0;
    for (size_t i = 0; i < gates.size(); ++i) {
        const GateProbe hit = probe_gate(gates[i], x, z);
        if (!hit.inside) continue;
        const int32_t entry = hit.along_track + hit.half_depth;  // 0 at the entry face
        if (best < 0 || entry < best_entry) {
            best = static_cast<int>(i);
            best_entry = entry;
        }
    }
    if (inside) *inside = best >= 0;
    if (best >= 0) return best;
    // Nothing contains it - true of a third of the shipped lines, which are laid between two
    // slabs or across a verge.
    //
    // ⚠️ Fall back on the nearest MIDDLE OF THE ROAD, not the nearest anchor: an anchor is a
    // gate's across-track origin, i.e. a point on the VERGE, up to half the gate's width away
    // from the road. Measuring to it picks the wrong gate on a wide circuit.
    int64_t nearest = 0;
    for (size_t i = 0; i < gates.size(); ++i) {
        const RespawnPoint mid = gate_respawn(gates[i], 0x2000);
        const int64_t dx = x - mid.x, dz = z - mid.z;
        const int64_t d2 = dx * dx + dz * dz;
        if (best < 0 || d2 < nearest) {
            best = static_cast<int>(i);
            nearest = d2;
        }
    }
    return best;
}

void mark_checkpoint_gates(std::vector<Gate>& gates, std::vector<CheckLine>& lines) {
    if (gates.empty()) return;
    for (CheckLine& c : lines) {
        c.gate = gate_of_point(gates, c.x, c.z, &c.in_gate);
        if (c.gate >= 0) gates[static_cast<size_t>(c.gate)].checkpoint = true;
    }
    // Gate 0 is the start/finish line: it is always one, whatever the decor says.
    gates[0].checkpoint = true;
}

bool check_line_touched(const CheckLine& line, const VehicleSim& car) {
    if (!car.tune) return false;
    const VehicleBox& body = car.tune->box;
    const int32_t x = car.world_x(), y = car.world_y(), z = car.world_z();

    // The original's height test, for category 54 only. +Y is DOWN: the underside of the car has to be below
    // the top of the box, or the car went over it. The second half of the original's test -
    // that the box's top is not buried under the ground the car stands on - is not ported: it
    // needs the terrain height, and no shipped line is buried.
    if (line.height_gated && y + body.ybottom < line.y + line.y_top) return false;

    const int32_t dx = line.x - x, dz = line.z - z;
    if (dx > 0x1000 || dx < -0x1000 || dz > 0x1000 || dz < -0x1000) return false;

    // The original classifies the body's corners against the box's four bounds. What it
    // answers is "do the two rectangles overlap", and that is asked here by separating axis,
    // the same way the engine's blocking contact does (sim/vehicle.cpp) - a box 100 thick is
    // easily straddled by a car, and a corner test alone would miss that.
    const int32_t oyaw = line.yaw_bam16 & 0xFFFF;
    const int32_t vyaw = car.yaw_bam16() & 0xFFFF;
    const int32_t o_cx = (line.box_xmin + line.box_xmax) / 2, o_cz = (line.box_zmin + line.box_zmax) / 2;
    const int32_t o_hx = (line.box_xmax - line.box_xmin) / 2, o_hz = (line.box_zmax - line.box_zmin) / 2;
    const int32_t b_cx = (body.xmin + body.xmax) / 2, b_cz = (body.zmin + body.zmax) / 2;
    const int32_t b_hx = (body.xmax - body.xmin) / 2, b_hz = (body.zmax - body.zmin) / 2;
    // Local +X of a frame turned by `yaw` is heading yaw + 0x4000, local +Z is heading yaw, and
    // a heading h is the world direction (sin h, cos h).
    const int32_t oax_x = fx::sin16(oyaw + 0x4000), oax_z = fx::cos16(oyaw + 0x4000);
    const int32_t oaz_x = fx::sin16(oyaw), oaz_z = fx::cos16(oyaw);
    const int32_t bax_x = fx::sin16(vyaw + 0x4000), bax_z = fx::cos16(vyaw + 0x4000);
    const int32_t baz_x = fx::sin16(vyaw), baz_z = fx::cos16(vyaw);
    const int32_t tx = (line.x + fx::mul15(o_cx, oax_x) + fx::mul15(o_cz, oaz_x)) -
                       (x + fx::mul15(b_cx, bax_x) + fx::mul15(b_cz, baz_x));
    const int32_t tz = (line.z + fx::mul15(o_cx, oax_z) + fx::mul15(o_cz, oaz_z)) -
                       (z + fx::mul15(b_cx, bax_z) + fx::mul15(b_cz, baz_z));
    const int32_t axes[4][2] = {{oax_x, oax_z}, {oaz_x, oaz_z}, {bax_x, bax_z}, {baz_x, baz_z}};
    auto absm = [](int32_t v) { return v < 0 ? -v : v; };
    for (const auto& a : axes) {
        const int32_t ro = absm(fx::mul15(o_hx, fx::mul15(oax_x, a[0]) + fx::mul15(oax_z, a[1]))) +
                           absm(fx::mul15(o_hz, fx::mul15(oaz_x, a[0]) + fx::mul15(oaz_z, a[1])));
        const int32_t rb = absm(fx::mul15(b_hx, fx::mul15(bax_x, a[0]) + fx::mul15(bax_z, a[1]))) +
                           absm(fx::mul15(b_hz, fx::mul15(baz_x, a[0]) + fx::mul15(baz_z, a[1])));
        const int32_t d = fx::mul15(tx, a[0]) + fx::mul15(tz, a[1]);
        if (ro + rb - absm(d) <= 0) return false;   // a separating axis
    }
    return true;
}

GateProbe probe_gate(const Gate& g, int32_t x, int32_t z) {
    GateProbe p;
    const int32_t dx = x - g.x;
    const int32_t dz = z - g.z;
    // The original's early-out, and it is a real constraint on the data: no gate ever reaches
    // more than 0x5800 from its anchor in either axis.
    if (dx > 0x5800 || dx < -0x5800 || dz > 0x5800 || dz < -0x5800) return p;

    const int32_t idx = g.angle & 0x3FF;
    const int32_t sn = tbl_sin(idx), cs = tbl_cos(idx);

    // The original's gate test, step for step. `across` is the one that runs the way the car
    // drives - the vehicle yaw the respawn writes, (angle << 6) + 0x4000, is exactly this axis.
    p.across_track = fx::mul15(dz, cs) + fx::mul15(dx, sn);
    if (p.across_track < 0 || p.across_track > g.length) return p;

    p.along_track = fx::mul15(dx, cs) - fx::mul15(dz, sn);
    // The slab is a trapezoid seen from above: its half-depth runs from `half_depth` at one side
    // of the road to `half_depth + half_depth_delta` at the other, linearly. That is what makes
    // a gate on a bend deeper on the outside of the curve than on the inside.
    p.half_depth = g.half_depth + div_trunc(static_cast<int64_t>(g.half_depth_delta) * p.across_track,
                                            g.length);
    if (p.half_depth <= 0) return p;  // the original would divide by zero here; no shipped gate does
    if (p.along_track > p.half_depth || p.along_track < -p.half_depth) return p;

    p.inside = true;
    // 0 at the entry face, 0x4000 at the exit face, 0x2000 half way through - the figure the
    // respawn reads back to put a car where it was in the gate.
    p.lateral = div_trunc(static_cast<int64_t>(p.along_track) << 13, p.half_depth) + 0x2000;
    return p;
}

void gate_point(const Gate& g, int32_t across_track, int32_t along_track, int32_t& x,
                int32_t& z) {
    const int32_t idx = g.angle & 0x3FF;
    const int32_t sn = tbl_sin(idx), cs = tbl_cos(idx);
    // The original respawn's own two expressions, and the exact inverse of probe_gate above.
    x = g.x + fx::mul15(along_track, cs) + fx::mul15(across_track, sn);
    z = g.z - fx::mul15(along_track, sn) + fx::mul15(across_track, cs);
}

int32_t gate_half_depth_at(const Gate& g, int32_t across_track) {
    if (g.length <= 0) return g.half_depth;
    return g.half_depth +
           div_trunc(static_cast<int64_t>(g.half_depth_delta) * across_track, g.length);
}

RespawnPoint gate_respawn(const Gate& g, int32_t lateral) {
    // ⚠️ `length / 2` is THE MIDDLE OF THE ROAD, not the middle of a corridor: `length` spans
    // the gate ACROSS the track. That is the whole of the original's placement rule, and it is
    // why it never drops a car on the verge. The `lateral` it keeps only decides how far
    // THROUGH the gate the car reappears, along the direction of travel.
    const int32_t across_track = g.length / 2;
    const int32_t half = g.half_depth + g.half_depth_delta / 2;
    const int32_t along_track =
        static_cast<int32_t>((static_cast<int64_t>(half) * (lateral - 0x2000)) / 0x2000);

    RespawnPoint r;
    gate_point(g, across_track, along_track, r.x, r.z);
    r.yaw_bam16 = gate_yaw_bam16(g);
    return r;
}

namespace {

// The world position of a grid-frame offset: the original's own two expressions, shared by
// grid_slot() and the validation of the extended places below.
void grid_to_world(const StartingGrid& grid, int32_t a, int32_t b, int32_t& x, int32_t& z) {
    const int32_t idx = (grid.yaw_bam16 >> 6) & 0x3FF;
    const int32_t cs = tbl_cos(idx), sn = tbl_sin(idx);
    x = grid.x + static_cast<int32_t>(static_cast<int16_t>(
                     (static_cast<int64_t>(a) * cs + static_cast<int64_t>(b) * sn) / 32768));
    z = grid.z + static_cast<int32_t>(static_cast<int16_t>(
                     (static_cast<int64_t>(b) * cs - static_cast<int64_t>(a) * sn) / 32768));
}

// The room kept between the level's six places and a copy of them.
constexpr int32_t kGridGap = 512;

// How far the six places spread along the grid heading, and across it.
int32_t extent_along(const StartingGrid& g) {
    int32_t lo = g.six[0].b, hi = g.six[0].b;
    for (const StartingGrid::Slot& s : g.six) {
        lo = std::min(lo, s.b);
        hi = std::max(hi, s.b);
    }
    return hi - lo;
}

int32_t extent_across(const StartingGrid& g) {
    int32_t lo = g.six[0].a, hi = g.six[0].a;
    for (const StartingGrid::Slot& s : g.six) {
        lo = std::min(lo, s.a);
        hi = std::max(hi, s.a);
    }
    return hi - lo;
}

} // namespace

void extend_grid(StartingGrid& grid, const SimTerrain& terrain, const VehicleTuning* probe) {
    grid.extra_count = 0;
    if (!grid.present || !terrain.valid()) return;
    const StartingGrid::Slot* six = grid.six;

    auto ground = [&](int32_t x, int32_t z) { return terrain.height_fx(x << 9, z << 9) >> 9; };
    int32_t x0 = 0, z0 = 0;
    grid_to_world(grid, six[0].a, six[0].b, x0, z0);
    const int32_t g0 = ground(x0, z0);

    // How far the probe drifts, squared, left alone on a place for 60 ticks.
    auto drift2 = [&](int32_t a, int32_t b, int32_t dyaw) -> int64_t {
        RespawnPoint r;
        grid_to_world(grid, a, b, r.x, r.z);
        r.yaw_bam16 = (grid.yaw_bam16 + dyaw) & 0xFFFF;
        VehicleSim car;
        car.reset(*probe, terrain, r, NitroReserve{});
        car.place_on_grid();
        const int64_t x0 = car.world_x(), y0 = car.world_y(), z0 = car.world_z();
        for (int t = 0; t < 60; ++t) car.step(VehicleInput{});
        const int64_t dx = car.world_x() - x0, dy = car.world_y() - y0, dz = car.world_z() - z0;
        return dx * dx + dy * dy + dz * dz;
    };
    int64_t drift_limit2 = 0;
    if (probe) {
        int64_t d0 = 0;
        const int64_t base2 = drift2(six[0].a, six[0].b, six[0].dyaw);
        while (d0 * d0 < base2) ++d0;   // an integer square root, rounded up
        drift_limit2 = (d0 + 40) * (d0 + 40);
    }

    // Everything already on the grid, in world units, to keep the new places clear of it.
    std::vector<std::pair<int32_t, int32_t>> taken;
    for (int i = 0; i < 6; ++i) {
        int32_t x = 0, z = 0;
        grid_to_world(grid, six[i].a, six[i].b, x, z);
        taken.emplace_back(x, z);
    }
    // A place is good when the ground is level with place 0's and flat under a car, when no
    // obstacle stands within reach, and when no other place is too close. Integer only: every
    // machine of a network race must pick the same places.
    auto good = [&](int32_t a, int32_t b) {
        int32_t x = 0, z = 0;
        grid_to_world(grid, a, b, x, z);
        if (x < 0 || z < 0 || x >= terrain.w * 512 || z >= terrain.h * 512) return false;
        const int32_t g = ground(x, z);
        if (g - g0 > 160 || g0 - g > 160) return false;
        int32_t lo = g, hi = g;
        for (int k = 0; k < 4; ++k) {
            const int32_t h = ground(x + ((k & 1) ? 160 : -160), z + ((k & 2) ? 160 : -160));
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (hi - lo > 120) return false;
        // Clear of anything that pushes: an obstacle within 350 units, a ramp or a platform
        // within 400 (measured: at 250 a car put there was shoved 100 to 750 units off it).
        for (const SimObject& o : terrain.blockers) {
            const int64_t dx = o.x - x, dz = o.z - z;
            if (dx * dx + dz * dz < 350LL * 350) return false;
        }
        for (const SimObject& o : terrain.objects) {
            const int64_t dx = o.x - x, dz = o.z - z;
            if (dx * dx + dz * dz < 400LL * 400) return false;
        }
        for (const auto& t : taken) {
            const int64_t dx = t.first - x, dz = t.second - z;
            if (dx * dx + dz * dz < 400LL * 400) return false;
        }
        return true;
    };
    auto accept = [&](int32_t a, int32_t b, int32_t dyaw) {
        if (grid.extra_count >= 6 || !good(a, b)) return;
        if (probe && drift2(a, b, dyaw) > drift_limit2) return;
        grid.extra[grid.extra_count++] = StartingGrid::Slot{a, b, dyaw};
        int32_t x = 0, z = 0;
        grid_to_world(grid, a, b, x, z);
        taken.emplace_back(x, z);
    };
    // ⚠️ THE GRIDS ARE NOT ALL TWO COLUMNS. Most are two staggered columns running back from
    // the line, but CITY3's six stand in ONE ROW across the road and MOON1's in a fan. So the
    // six are not "continued" column by column - which stacked places on top of each other on
    // those two - but COPIED AS A BLOCK: the same shape again, behind the first, then beside
    // it, then further behind, keeping only the places the ground allows.
    const int32_t depth = extent_along(grid) + kGridGap;
    const int32_t width = extent_across(grid) + kGridGap;
    for (int p = 0; p < 6; ++p) accept(six[p].a, six[p].b - depth, six[p].dyaw);
    for (int p = 0; p < 6; ++p) accept(six[p].a - width, six[p].b, six[p].dyaw);
    for (int p = 0; p < 6; ++p) accept(six[p].a + width, six[p].b, six[p].dyaw);
    for (int p = 0; p < 6; ++p) accept(six[p].a, six[p].b - 2 * depth, six[p].dyaw);
    // Whatever is still missing, grid_slot() takes from the plain copy behind - a place on a
    // slope is better than none - but max_grid_racers() does not count it, so a lobby never
    // opens that many seats.
}

int max_grid_racers(const StartingGrid& grid) { return 6 + grid.extra_count; }

RespawnPoint grid_slot(const StartingGrid& grid, int racers, int slot) {
    RespawnPoint r;
    r.x = grid.x;
    r.z = grid.z;
    r.yaw_bam16 = grid.yaw_bam16 & 0xFFFF;
    if (!grid.present) return r;

    // `racers == 2 ? slot * 3 : slot * 3 + 6` - exactly two racers take the two-player
    // triplets, everything else takes the six.
    const StartingGrid::Slot* table = (racers == 2) ? grid.two : grid.six;
    const int count = (racers == 2) ? 2 : 6;
    if (slot < 0) slot = 0;
    StartingGrid::Slot s = table[slot < count ? slot : count - 1];
    // ⚠️ PAST THE SIXTH PLACE, OURS AND NOT THE ORIGINAL'S (the network game's twelve): the
    // places extend_grid() checked against the terrain, else the six copied as a block right
    // behind themselves - never interpolated between two places, which would stand a car
    // inside its neighbours.
    if (racers != 2 && slot >= count && slot - count < grid.extra_count) {
        s = grid.extra[slot - count];
    } else if (racers != 2 && slot >= count) {
        const StartingGrid::Slot& base = grid.six[(slot - count) % 6];
        s.a = base.a;
        s.b = base.b - (1 + (slot - count) / 6) * (extent_along(grid) + kGridGap);
        s.dyaw = base.dyaw;
    }

    const int32_t idx = (grid.yaw_bam16 >> 6) & 0x3FF;
    const int32_t cs = tbl_cos(idx), sn = tbl_sin(idx);
    // The original's own two expressions. It sums the products FIRST and divides the sum by
    // 0x8000, then truncates the result to a signed 16-bit offset before adding it to the grid
    // origin - so a slot can never be more than 32767 units off it.
    //
    // ⚠️ `a` multiplies the table read at t+256 (the COSINE) and `b` the one at t
    // (the SINE). This was ported with the two swapped, which turned the grid a
    // quarter turn: the six places ran ACROSS the road from the origin instead of two staggered
    // columns running back from the line, and the car started mid-grid. Read against the
    // painted `S_LINES` blocks with physics_bench's GRILLE block: `a` is across, `b` is along.
    const int32_t ox = static_cast<int32_t>(
        static_cast<int16_t>((static_cast<int64_t>(s.a) * cs + static_cast<int64_t>(s.b) * sn) /
                             32768));
    const int32_t oz = static_cast<int32_t>(
        static_cast<int16_t>((static_cast<int64_t>(s.b) * cs - static_cast<int64_t>(s.a) * sn) /
                             32768));
    r.x = grid.x + ox;
    r.z = grid.z + oz;
    r.yaw_bam16 = (grid.yaw_bam16 + s.dyaw) & 0xFFFF;
    return r;
}

// ------------------------------------------------------------------------------- the race state

void RaceState::configure(std::vector<Gate> gates, int laps, int countdown_ticks, int racers,
                          std::vector<CheckLine> lines) {
    gates_ = std::move(gates);
    lines_ = std::move(lines);
    last_ordinal_ = -1;
    for (const CheckLine& c : lines_) last_ordinal_ = std::max(last_ordinal_, c.ordinal());
    laps_ = laps > 0 ? laps : kDefaultLaps;
    countdown_start_ = countdown_ticks > 0 ? countdown_ticks : kDefaultCountdownTicks;
    progress_.assign(static_cast<size_t>(racers > 0 ? racers : 1), RacerProgress());
    restart();
}

void RaceState::restart() {
    countdown_ = countdown_start_;
    race_ticks_ = 0;
    beeped_ = false;
    over_ = false;
    for (size_t i = 0; i < progress_.size(); ++i) {
        progress_[i] = RacerProgress();
        progress_[i].place = static_cast<int>(i) + 1;
        arm_checks(progress_[i]);
    }
}

// Puts a racer's checkpoint set back to "none passed".
//
// Two gates are left OUT of the count:
//   * gate 0 is the line itself - crossing it is the event being validated, not a condition;
//   * an `unsafe` gate records no progress at all (see update_progress), so requiring one would
//     be incoherent - the lap could never complete. CITY1 has exactly that case: its gate 16 is
//     both `unsafe` and the gate the start-line gantry falls in.
//
// ⚠️ Only on a level WITHOUT `CHECKP` lines. With them, the lines' own order is the whole rule
// (touch_line) and this set is left empty.
void RaceState::arm_checks(RacerProgress& p) const {
    p.checks_seen.assign(gates_.size(), 0);
    p.checks_missing = 0;
    if (!lines_.empty()) {
        p.checks_missing = last_ordinal_;   // every line but the start/finish one
        return;
    }
    for (size_t i = 1; i < gates_.size(); ++i)
        if (gates_[i].checkpoint && gates_[i].safe) ++p.checks_missing;
}

int RaceState::expected_line_gate(const RacerProgress& p) const {
    if (lines_.empty() || p.last_check == 0x80) return -1;   // not started: nothing to miss
    return line_gate((p.last_check & 0x80) ? 0 : (p.last_check & 0x7f) + 1);
}

int RaceState::line_gate(int ordinal) const {
    // Several instances can share the ordinal - pieces of one line. The first one CONTAINED in a
    // gate is the better answer; a line lying outside the ring only has a nearest gate.
    int fallback = -1;
    for (const CheckLine& c : lines_) {
        if (c.ordinal() != ordinal || c.gate < 0) continue;
        if (c.in_gate) return c.gate;
        if (fallback < 0) {
            // Not inside any gate: the nearest one may lie BEYOND the line. One gate back is
            // certainly before it, which is where "juste devant" has to be.
            const int n = static_cast<int>(gates_.size());
            fallback = (c.gate - 1 + n) % n;
        }
    }
    return fallback;
}

int RaceState::countdown_number() const {
    if (countdown_ <= -70) return -1;  // GO has been up for a second: nothing left to show
    if (countdown_ <= 0) return 0;     // GO
    const int n = (countdown_ + 69) / 70;
    return n > 3 ? 3 : n;
}

RespawnPoint RaceState::start_of(const StartingGrid& grid, int slot) const {
    return grid_slot(grid, racer_count(), slot);
}

int RaceState::respawn_gate(int racer) const {
    if (racer < 0 || racer >= racer_count() || gates_.empty()) return -1;
    const RacerProgress& p = progress_[static_cast<size_t>(racer)];
    if (p.reached < 0) return -1;  // never reached a gate: the grid is still the answer
    const int n = static_cast<int>(gates_.size());

    // ⚠️ A MISSED CHECKPOINT COMES FIRST - the user's description of the original: "si on en
    // avait raté un, le jeu nous le disait et ALT nous ramenait juste devant". Missed means
    // BEHIND: a sounding gate the racer has already driven past the position of, this lap,
    // without ever being registered in it. Walking back from `reached` finds the nearest one,
    // which is the one to be put back at.
    //
    // The gates ahead are not candidates - not having reached them yet is the normal state of
    // a lap, and treating that as "missed" would teleport a car FORWARD for pressing ALT.
    //
    // With `CHECKP` lines, "missed" is the NEXT LINE IN ORDER lying behind the car: its gate is
    // less than half the ring behind `reached`. The original gets there through its gate window
    // - progress is not recorded past the gate the last line's offset-21 byte names,
    // so its respawn never lands beyond the next line.
    //
    // ⚠️ NOT "less than half the ring behind". That was the first rule, and it failed exactly
    // where the user pressed ALT: on the banner. RIG1 has three lines for 24 gates, so when its
    // last line is skipped the finish line - where the banner goes up - is 14 gates further
    // on, past half the ring, and ALT fell back on the car's own gate: just behind the line
    // showing MISSED CHECK. Two tests instead:
    //   * a line AHEAD of the expected one has been touched (RacerProgress::skipped_line): the
    //     expected one was driven past, whatever the distance;
    //   * otherwise, measured FROM THE LAST LINE TAKEN: the car is past the expected line if it
    //     is further round from that line than the expected line is - and not so much further
    //     that it is really behind the last line, reversing.
    if (!lines_.empty()) {
        int g = expected_line_gate(p);
        if (g >= 0) {
            bool passed = p.skipped_line;
            const int prev = line_gate(p.last_check & 0x7f);
            if (!passed && prev >= 0) {
                const int ahead_line = (g - prev + n) % n;
                const int ahead_car = (p.reached - prev + n) % n;
                passed = ahead_car > ahead_line && ahead_car <= ahead_line + (n - ahead_line) / 2;
            }
            if (passed) {
                for (int k = 0; k < n; ++k) {
                    if (gates_[static_cast<size_t>(g)].safe) return g;
                    g = (g - 1 + n) % n;
                }
            }
        }
    }
    for (int k = 1; k <= p.reached && lines_.empty(); ++k) {
        const int g = p.reached - k;
        if (g <= 0) break;                       // gate 0 is the line, not a condition
        if (!gates_[static_cast<size_t>(g)].checkpoint) continue;
        if (static_cast<size_t>(g) < p.checks_seen.size() && p.checks_seen[static_cast<size_t>(g)])
            continue;
        if (gates_[static_cast<size_t>(g)].safe) return g;
    }

    int g = p.reached;
    for (int k = 0; k < n; ++k) {
        if (gates_[static_cast<size_t>(g)].safe) return g;
        g = (g - 1 + n) % n;
    }
    return -1;  // every gate is unsafe: no shipped level does that
}

RespawnPoint RaceState::respawn_for(int racer, const RespawnPoint& fallback) const {
    const int gate = respawn_gate(racer);
    if (gate < 0) return fallback;
    const RacerProgress& p = progress_[static_cast<size_t>(racer)];
    {
        // The original's own point, and it needs no deviation: `length / 2` is the middle of
        // the ROAD, so keeping the racer's `lateral` only decides how far through the gate it
        // reappears. (An earlier version forced 0x2000 here, to stop the respawn dropping the
        // car on the verge - but the verge was the quarter-turn error in probe_gate, not this.)
        // The "is the spot occupied" retry is respawn_choice() below.
        //
        // On a MISSED checkpoint the lateral is not kept: the car goes to that gate's ENTRY
        // face, which is "juste devant" it - it then drives through the checkpoint it skipped
        // instead of starting past it.
        const int32_t lateral = (gate == p.reached) ? p.reached_lateral : 0;
        return gate_respawn(gates_[static_cast<size_t>(gate)], lateral);
    }
}

RaceState::RespawnChoice RaceState::respawn_choice(int racer, const RespawnPoint& fallback,
                                                   SpotTaken occupied, const void* ctx) const {
    RespawnChoice out;
    int gate = respawn_gate(racer);
    if (gate < 0) {
        out.at = fallback;
        return out;
    }
    const RacerProgress& p = progress_[static_cast<size_t>(racer)];
    const int n = static_cast<int>(gates_.size());
    // The original works on `lateral - 0x2000`: -0x2000 is the entry face, +0x2000 the exit.
    int32_t along = ((gate == p.reached) ? p.reached_lateral : 0) - 0x2000;
    out.gate = gate;
    out.at = gate_respawn(gates_[static_cast<size_t>(gate)], along + 0x2000);
    if (!occupied) return out;
    // The original loops for as long as it takes; a field of twelve cannot fill a whole ring,
    // but a bound keeps a degenerate level (every gate unsafe but one, and that one full) from
    // hanging - it then keeps the first spot, as if nothing were checked.
    const RespawnChoice first = out;
    for (int tries = 0; tries < 64 * n + 64; ++tries) {
        if (!occupied(out.at, ctx)) return out;
        along -= 0x100;
        if (along < -0x2000) {
            // The exit face of the previous gate whose safe byte is set.
            along = 0x2000;
            int k = 0;
            do {
                gate = (gate - 1 + n) % n;
            } while (!gates_[static_cast<size_t>(gate)].safe && ++k < n);
        }
        out.gate = gate;
        out.at = gate_respawn(gates_[static_cast<size_t>(gate)], along + 0x2000);
    }
    return first;
}

void RaceState::on_respawned(int racer) { on_respawned(racer, respawn_gate(racer)); }

void RaceState::on_respawned(int racer, int gate) {
    if (gate < 0 || racer < 0 || racer >= racer_count()) return;
    RacerProgress& p = progress_[static_cast<size_t>(racer)];
    // The car is now AT that gate, so the ring has to say so. Without this the return trip -
    // and everything after it - reads as driving backwards: `reached` would still be further on,
    // every probe would be a reversal, and the stretch could never be registered again.
    p.gate = gate;
    p.reached = gate;
    p.lateral = p.reached_lateral = 0x2000;
    p.prev_gate = gate;
    p.prev_lateral = 0x2000;
    p.wrong_way_ticks = 0;   // being put back on the track is not driving the wrong way
    p.wrong_way = false;
}

int32_t RaceState::progress_permille(int racer) const {
    if (racer < 0 || racer >= racer_count() || gates_.empty()) return 0;
    const RacerProgress& p = progress_[static_cast<size_t>(racer)];
    if (p.reached < 0) return 0;
    const int n = static_cast<int>(gates_.size());
    const int64_t step = (static_cast<int64_t>(p.reached) * 1000) / n;
    return static_cast<int32_t>(step + (static_cast<int64_t>(p.along_permille) / n));
}

void RaceState::update_progress(int racer, const VehicleSim& car) {
    RacerProgress& p = progress_[static_cast<size_t>(racer)];
    const int n = static_cast<int>(gates_.size());
    if (n == 0) return;

    const int32_t x = static_cast<int32_t>(car.px >> 9);
    const int32_t z = static_cast<int32_t>(car.pz >> 9);

    // Which gate holds the car. Gates OVERLAP - consecutive ones share several thousand units,
    // which is what makes the ring watertight round a bend - so the order of the search decides
    // the answer. The original scans FORWARD FROM THE GATE THE CAR WAS IN and takes the first
    // hit (its first loop), which is what is reproduced here.
    int found = -1;
    int32_t lateral = p.lateral;
    // ⚠️ THE GATE WINDOW FIRST - the original's first loop (ported 2026-09-27). Once a line has
    // been taken, the search runs over [window_start, window] only, from the car's own gate if
    // it is in there, and only a gate found THERE is progress. Gates overlap and roads cross:
    // CITY1's grid lies both in gate 40 (its road) and in gate 16 (the bridge over it), and a
    // search of the whole ring, started from the grid's gate 16, kept the car "in 16" and
    // refused gates 0 to 15 as half a ring backwards - the placings (and so the rubber band)
    // stood still for the first third of lap 1.
    //
    // ⚠️ Read ROUND THE RING. The original steps `g + 1` and falls back to the start past the
    // end, so a window that straddles gate 0 (the one after the start line, [30, 5] on DIRT1)
    // only ever tests its first gate there, and its progress stands still until the next line.
    // Walking it modulo the ring is a deliberate difference; it changes nothing elsewhere.
    const bool windowed = !lines_.empty() && p.last_check != 0x80 && p.window_start < n &&
                          p.window < n;
    bool in_window = false;
    if (windowed) {
        const int ws = p.window_start;
        const int span = (p.window - ws + n) % n + 1;
        const int at = (p.gate >= 0 && (p.gate - ws + n) % n < span) ? (p.gate - ws + n) % n : 0;
        for (int k = 0; k < span; ++k) {
            const int g = (ws + (at + k) % span) % n;
            const GateProbe hit = probe_gate(gates_[static_cast<size_t>(g)], x, z);
            if (hit.inside) {
                found = g;
                lateral = hit.lateral;
                in_window = true;
                break;
            }
        }
    }
    // Then the whole ring - the original's second loop. What it finds places the car (the
    // overlay, the wrong-way count, the missed-line test in tick()), but once a line has been
    // taken it records NO progress: that is the window's whole point.
    if (found < 0) {
        const int from = (p.gate >= 0) ? p.gate : 0;
        for (int k = 0; k < n; ++k) {
            const int g = (from + k) % n;
            const GateProbe hit = probe_gate(gates_[static_cast<size_t>(g)], x, z);
            if (hit.inside) {
                found = g;
                lateral = hit.lateral;
                break;
            }
        }
    }
    if (found < 0) {
        p.gate = -1;  // off the ring entirely: `reached` stays where it was
        return;
    }

    p.gate = found;
    p.lateral = lateral;
    // How far THROUGH the gate the car is, which is the placing's tie-break. That is the
    // along-track coordinate, i.e. exactly what `lateral` already holds on 0..0x4000 - not the
    // across-track one, which only says where on the road's width the car sits.
    p.along_permille = div_trunc(static_cast<int64_t>(lateral) * 1000, 0x4000);
    if (windowed && !in_window) return;

    // ⚠️ AN `unsafe` GATE RECORDS NOTHING. The flag is not only about respawning: the original
    // tests it before the whole progress-recording block and
    // leaves without touching anything when it is clear. A gate laid over a jump or a bridge
    // mouth therefore does not advance a lap - which is what stops a car in mid-air from
    // registering the far side of a gap it has not driven.
    if (!gates_[static_cast<size_t>(found)].safe) return;

    // The first gate inside the window after the race's first line: progress, whatever its
    // number (the best-lap marker at -1 - see touch_line).
    if (p.take_next_gate) {
        p.take_next_gate = false;
        p.reached = found;
        p.reached_lateral = lateral;
        p.gate_crossed = true;
        return;
    }
    if (p.reached < 0) {
        // The first gate the car is ever seen in. On the grid that is the one BEFORE the start
        // line, so the run up to the line is not a lap.
        p.reached = found;
        p.reached_lateral = lateral;
        return;
    }
    if (found == p.reached) {
        p.reached_lateral = lateral;
        return;
    }

    // ⚠️ PROGRESS IS NOT "THE NEXT GATE". A long gate can cover the whole of the one after it -
    // CHOPPER1's are 10 714 units and its anchors 4 000 apart - so a car can leave gate `i`
    // already inside gate `i+2`, and a rule that only accepted `i+1` would stall the whole lap.
    // The original accepts ANY gate further on: its test is `best_gate < found`, not
    // `best_gate + 1 == found`. That was measured, not argued - a probe walked round every
    // circuit's gates in order registered 15 of DIRT1's 31 and 3 of CHOPPER1's 34 under the
    // strict rule, and all of them under this one. The bench's RACE block still runs that walk.
    //
    // Going BACKWARDS must not count, so the move is read round the ring and a step of more
    // than half the ring is a reversal, not a jump.
    const int delta = (found - p.reached + n) % n;
    if (delta == 0 || delta > n / 2) return;

    const bool wrapped = found < p.reached;  // came round past gate 0
    if (!lines_.empty()) {
        // The level has `CHECKP` lines: they, and only they, fire the checkpoints and the laps
        // (touch_line). The ring still places the racer, which is all that is left for it here.
        p.reached = found;
        p.reached_lateral = lateral;
        p.gate_crossed = true;
        return;
    }
    // ⚠️ THE BEEP IS FOR THE WHOLE SPAN, not for the gate the car landed in. Same cause as the
    // progression rule above: a long gate swallows the next one, so the car leaves gate `i`
    // already inside gate `i+2` and `i+1` is never "the gate it is in". Asking only whether
    // `found` carries a gantry therefore MISSES most gantries - the user drove a whole lap of
    // SNOW1 for one beep and a whole lap of DIRT1 for none, which is exactly the shape of it.
    // Every gate stepped over has been passed, so every gantry on them has been passed.
    for (int k = 1; k <= delta; ++k) {
        if (gates_[static_cast<size_t>((p.reached + k) % n)].checkpoint) {
            p.checkpoint_crossed = true;
            break;
        }
    }

    // ⚠️ THE SOUND AND THE VALIDATION ASK DIFFERENT QUESTIONS, and only one of them is
    // satisfied by stepping over a gate.
    //
    //   * the BEEP is "did I go past that gantry", so it takes the whole span the car moved
    //     over this tick - a tick that jumps a short slab still went past its banner;
    //   * VALIDATING a checkpoint is "was I in it", so it takes only the gate the car is
    //     actually found in. Cutting a corner, which is how a checkpoint is really missed,
    //     steps over the slab without ever entering it - and must not count.
    //
    // In practice the two agree: the ring is a tiling of slabs 1 700 to 9 000 units deep and a
    // car covers some 450 a tick, so every gate on the road is entered. They part company
    // exactly where the rule is meant to bite.
    if (found != 0 && gates_[static_cast<size_t>(found)].checkpoint &&
        gates_[static_cast<size_t>(found)].safe &&
        static_cast<size_t>(found) < p.checks_seen.size() &&
        !p.checks_seen[static_cast<size_t>(found)]) {
        p.checks_seen[static_cast<size_t>(found)] = 1;
        if (p.checks_missing > 0) --p.checks_missing;
    }
    p.reached = found;
    p.reached_lateral = lateral;
    p.gate_crossed = true;
    if (!wrapped) return;

    // Gate 0 is the start/finish line, and the index wrapping from the end of the ring back to
    // its start is the crossing. What that crossing MEANS takes two more tests.

    // ⚠️ THE FIRST CROSSING STARTS LAP 1, it does not end one. The grid is behind gate 0, so a
    // car begins the race inside the LAST gate of the ring and its first move into gate 0 wraps
    // the index - which is a crossing of the line, but of course not a lap. The original gets
    // the same result from its search window (not ported); this is the same
    // fact stated directly. Reported by the user: "le 1er passage de la ligne de départ fait
    // passer le nombre de lap de 1 à 2".
    if (!p.line_crossed_once) {
        p.line_crossed_once = true;
        p.lap_start_tick = race_ticks_;
        arm_checks(p);
        return;
    }

    // ⚠️ A LAP ONLY COUNTS IF NO CHECKPOINT WAS MISSED - the user's report of the original's own
    // rule. Cutting the course therefore costs the lap rather than winning it. The set is
    // re-armed either way, so a missed lap is a lost lap and not a dead end.
    if (p.checks_missing > 0) {
        p.missed_check = true;
        p.missed_check_now = true;
        p.missed_check_tick = race_ticks_;
        p.lap_start_tick = race_ticks_;
        arm_checks(p);
        return;
    }

    complete_lap(p);
}

void RaceState::complete_lap(RacerProgress& p) {
    ++p.lap;
    p.lap_crossed = true;
    p.missed_check = false;
    p.lap_ticks.push_back(race_ticks_ - p.lap_start_tick);
    p.lap_start_tick = race_ticks_;
    arm_checks(p);
    if (p.lap >= laps_ && !p.finished) {
        p.finished = true;
        p.finish_tick = race_ticks_;
    }
}

// The original's line handler, called from the collision router when the body touches a
// `CHECKP` box.
//
// `num` is the line's ordinal byte, `last` the last line taken in order (0x80 before the first).
// A line is IN ORDER when it is the next ordinal, or when it is the start line (0) and the last
// one taken carried the lap's final flag. Then:
//   * if it is a DIFFERENT line from the last one - two pieces of the same line share the
//     ordinal and must not fire twice - it is a checkpoint (event 0x24), and on ordinal 0 a lap
//     (0x25) instead;
//   * from the 0x80 start state, ordinal 0 has the same low bits as 0x80, so crossing the line
//     the first time fires NOTHING: it arms lap 1. That is the grid-behind-the-line case, and
//     the original solves it with no special code at all.
// Out of order is event 0x26, MISSED CHECK - unless it is the very line last touched, which is
// what makes brushing along a line, or crossing back over the previous one, silent.
void RaceState::touch_line(RacerProgress& p, const CheckLine& line) {
    const int num = line.number;
    const int last = p.last_check;
    enum { kNone, kCheck, kLap, kMissed } ev = kNone;
    if ((num & 0x7f) == last + 1 || (num == 0 && (last & 0x80) != 0)) {
        if ((num & 0x3f) != (last & 0x3f)) {
            if (last != 0x80) ev = kCheck;
            if (num == 0) {
                if (last != 0x80) ev = kLap;
                if (!p.line_crossed_once) {
                    // Only reachable if a lap's last line was taken before the first crossing
                    // - which the ordinals forbid. Kept so the lap clock can never run unset.
                    p.line_crossed_once = true;
                    p.lap_start_tick = race_ticks_;
                }
            }
        } else if (num == 0 && last == 0x80) {
            // The first crossing of the line: lap 1 starts now.
            p.line_crossed_once = true;
            p.lap_start_tick = race_ticks_;
        }
        // As in the original: the window's start takes its end, and its end the line's
        // offset-21 byte.
        p.window_start = p.window;
        p.window = line.window;
        // The FIRST line of the race: the original's best-lap marker goes to -1 (in its
        // full-search path), so the first gate then found inside the
        // new window is taken as progress whatever its number. That is what lets a car whose
        // grid the search put in some OTHER gate (CITY1's, under a bridge that is gate 16)
        // start counting from the road it is really on.
        if (last == 0x80) p.take_next_gate = true;
        p.last_check = static_cast<uint8_t>(num);
        p.missed_check = false;
        p.skipped_line = false;
        p.checks_missing = (num & 0x80) ? 0 : last_ordinal_ - (num & 0x7f);
    } else {
        ev = kMissed;
        // AHEAD or BEHIND the line expected? Ahead means that line was driven past - the case
        // ALT has to bring the car back for. Behind (reversing over a line already taken) must
        // not, or ALT would throw the car FORWARD. The start line (0) is the lap's last: it is
        // ahead of anything but itself.
        // ⚠️ NOT THE LINE JUST TAKEN. A body spans a line for several ticks as it crosses it,
        // so on the tick after taking line N it touches line N again - "out of order" by the
        // test above, and silenced as an event by the `last_touched` rule below. It said
        // nothing about a line being skipped, yet it armed `skipped_line`, and ALT then sent the
        // car to the NEXT line, ahead of it: after the lap's last line, that is the finish line
        // (user, 2026-09-27: "ALT m'a ramené juste avant la ligne d'arrivée"). Measured on the
        // ghosts' races: 20 to 60 % of positions would have been sent forward.
        if (last != 0x80 && (num & 0x7f) != (last & 0x7f)) {
            const int want = (last & 0x80) ? 0 : (last & 0x7f) + 1;
            const int got = num & 0x7f;
            if (got == 0 ? want != 0 : got > want) p.skipped_line = true;
        }
    }
    if (ev == kMissed && p.last_touched == num) ev = kNone;
    p.last_touched = static_cast<uint8_t>(num);

    switch (ev) {
        case kCheck: p.checkpoint_crossed = true; break;
        case kLap:
            // The lap's own sound replaces the checkpoint's, as event 0x25 replaces 0x24.
            if (!p.finished) complete_lap(p);
            p.checks_missing = last_ordinal_;
            break;
        case kMissed:
            p.missed_check = true;
            p.missed_check_now = true;
            p.missed_check_tick = race_ticks_;
            break;
        case kNone: break;
    }
}

void RaceState::tick(const VehicleSim* const* cars, int count) {
    beeped_ = false;
    for (RacerProgress& p : progress_) {
        p.gate_crossed = false;
        p.checkpoint_crossed = false;
        p.lap_crossed = false;
        p.missed_check_now = false;
        p.missed_window_now = false;
        p.wrong_way_now = false;
    }

    // One physics tick is TWO of the original's 14 ms timer ticks, and the countdown is counted
    // in those - like every other duration in the game.
    for (int half = 0; half < 2; ++half) {
        if (countdown_ > -1000) {
            --countdown_;
            // The original beeps at 210, 140, 70 and 0 ticks left: three seconds of count and
            // the green light. A circuit whose countdown is shorter than 210 simply starts
            // part way through, which is what SNOW3 (204) does.
            if (countdown_ == 210 || countdown_ == 140 || countdown_ == 70 || countdown_ == 0)
                beeped_ = true;
        }
        if (countdown_ < 0 && !over_) ++race_ticks_;
    }
    if (countdown_ >= 0) return;  // nothing has moved yet

    for (int i = 0; i < count && i < racer_count(); ++i) {
        if (!cars[i]) continue;
        update_progress(i, *cars[i]);
        // The `CHECKP` lines the body touches this tick, in instance order - the order the
        // original's spatial query hands them to its scenery pass is its own, but any FIXED order
        // gives the same result here, since two lines touched at once are either pieces of the
        // same one or the first of them is out of order anyway.
        RacerProgress& p = progress_[static_cast<size_t>(i)];
        for (const CheckLine& c : lines_)
            if (check_line_touched(c, *cars[i])) touch_line(p, c);
        // MISSED CHECK THE MOMENT THE LINE IS DRIVEN PAST - the original's second path (found
        // 2026-09-27; the user: "dans l'original le jeu nous le dit tout
        // de suite"). The last line taken set the window's end (its offset-21 byte) to
        // about the gate of the NEXT line. Stepping from that gate into the one after it - from
        // it, or from off the ring - means the next line was passed without being touched:
        // banner and event 0x28, `hnd fx check 4`, at once, instead of at the
        // next line touched out of order. Tested AFTER this tick's lines, so a line touched on
        // the very tick its gate is left has already moved the window on.
        const int n = static_cast<int>(gates_.size());
        if (!lines_.empty() && p.last_check != 0x80 && p.window < n && p.gate >= 0 &&
            p.gate != p.prev_gate && (p.prev_gate == p.window || p.prev_gate < 0) &&
            p.gate == (p.window + 1) % n) {
            p.missed_check = true;
            p.missed_window_now = true;
            p.missed_check_tick = race_ticks_;
            p.skipped_line = true;   // the expected line is behind: that is where ALT goes
        }
    }

    // Driving the wrong way - a check the original makes on every exit of its
    // progression function. It compares this tick's place on the ring with last tick's; going
    // backwards counts up, going forwards resets. See RacerProgress::wrong_way_ticks.
    // ⚠️ NOT MOVING IS NEITHER: when the (gate, lateral) pair is exactly last tick's, the
    // original returns before touching the count. This port used to read `lateral <=` there,
    // so a car standing still - a finished one, parked past the line - raised WRONG WAY after
    // two seconds (user, 2026-09-30).
    for (RacerProgress& p : progress_) {
        const int n = static_cast<int>(gates_.size());
        bool backwards = false;
        if (p.gate >= 0 && p.prev_gate >= 0 && p.gate == p.prev_gate &&
            p.lateral == p.prev_lateral)
            continue;   // prev_* already equal the current pair
        if (p.gate >= 0 && p.prev_gate >= 0) {
            if (p.gate == p.prev_gate) {
                backwards = p.lateral < p.prev_lateral;
            } else {
                // ⚠️ Read ROUND THE RING, not as a plain `<`. The original compares the two
                // indices directly, which calls the one legitimate wrap past gate 0 a reversal
                // for a single tick - harmless there because the threshold is 140 of them, and
                // avoided here for nothing.
                const int delta = (p.gate - p.prev_gate + n) % n;
                backwards = delta > n / 2;
            }
        }
        if (backwards && countdown_ < 0) {
            // Two of the original's 14 ms ticks per physics tick, like every other duration.
            p.wrong_way_ticks += 2;
            if (p.wrong_way_ticks >= kWrongWayTicks && !p.wrong_way) {
                p.wrong_way = true;
                p.wrong_way_now = true;
            }
        } else if (!backwards) {
            p.wrong_way_ticks = 0;
            p.wrong_way = false;
        }
        p.prev_gate = p.gate;
        p.prev_lateral = p.lateral;
    }

    // Placing: furthest round the ring leads. The key is integer and total, so every machine
    // sorts the field the same way - ties fall back on the grid order, never on address or
    // iteration chance.
    const int n = static_cast<int>(gates_.size());
    std::vector<std::pair<int64_t, int>> order;
    order.reserve(progress_.size());
    for (size_t i = 0; i < progress_.size(); ++i) {
        const RacerProgress& p = progress_[i];
        int64_t key = static_cast<int64_t>(p.lap) * (n > 0 ? n : 1) * 1024;
        if (p.reached >= 0) {
            key += static_cast<int64_t>(p.reached) * 1024;
            // How far into that gate it has got, so two cars in the same gate still sort.
            key += (static_cast<int64_t>(p.along_permille) * 1024) / 1000;
        }
        // A finished racer is ranked by WHEN it finished, ahead of anyone still running - and by
        // that ALONE. ⚠️ It used to be ADDED to the progress key, but a car keeps driving after
        // the line, and how far it got past it (up to gates x 1024) outweighed a few hundred
        // ticks between two finishes: the first ghost home was classed second. Found by the
        // bench's REPLAY race, the first one with several finishers.
        if (p.finished) key = (1LL << 40) - static_cast<int64_t>(p.finish_tick);
        order.emplace_back(key, static_cast<int>(i));
    }
    std::sort(order.begin(), order.end(), [](const std::pair<int64_t, int>& a,
                                             const std::pair<int64_t, int>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    for (size_t k = 0; k < order.size(); ++k)
        progress_[static_cast<size_t>(order[k].second)].place = static_cast<int>(k) + 1;

    // The race is over once every racer has run its laps - the original's own test, which walks
    // the seven vehicle records and compares each one's lap byte against the level's `laps`.
    bool all_done = true;
    for (const RacerProgress& p : progress_) if (!p.finished) { all_done = false; break; }
    over_ = all_done;
}

} // namespace bigred
