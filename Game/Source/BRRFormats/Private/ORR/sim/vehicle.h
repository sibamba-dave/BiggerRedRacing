// One vehicle, simulated exactly the way the original does it.
//
// The model, in short:
//
//   velocity lives in POLAR world form (speed + travel direction) and is decomposed into the
//   body frame (forward, lateral) at the top of every tick. The engine pushes `forward`; `svel`
//   drags `lateral` toward zero. That decay, and nothing else, is what makes the car turn.
//
// Everything here is integer. The simulation must stay bit-identical across machines for
// lockstep networking, so no float ever enters this file.
//
// All coordinates and angles are the ORIGINAL GAME'S, not the renderer's: +Y points DOWN,
// world X is not mirrored, positions are in 1/512 of a world unit. The renderer converts on the
// way out (VehicleSim::render_position), exactly like the decor does.
#pragma once

#include "../formats/level_script.h" // kPhysicsTickMs - the tick is a duration, not a count
#include "../formats/scenery.h"
#include "../formats/vehicle_script.h"
#include "fixed.h"
#include "race_rules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bigred {


// The world's destruction state: which decor instances have been knocked over, and when.
//
// It lives OUTSIDE VehicleSim because it is shared: the player and every recorded opponent
// write into the same one, and a cone flattened by an opponent has to be flattened for
// everybody. It is simulation state, not renderer state - part of what lockstep has to agree
// on - which is why the instant is a TICK INDEX and never a
// wall-clock reading.
struct SceneryState {
    std::vector<uint8_t> wrecked;     // 1 once knocked over; never goes back to 0
    std::vector<int32_t> wreck_tick;  // the simulation tick it happened on, for the flipbook
    // How many objects are currently playing their destruction animation. The original's list
    // holds 20: the 21st object destroyed while they are all busy skips the
    // animation and lands directly on its settled state. Kept because it is visible.
    static constexpr int kMaxAnimating = 20;
    int animating = 0;
    // Bumped by every wreck, so a renderer can tell "anything changed?" in one comparison.
    uint32_t revision = 0;

    void resize(size_t n) {
        wrecked.assign(n, 0);
        wreck_tick.assign(n, 0);
        animating = 0;
        ++revision;
    }
    bool is_wrecked(int index) const {
        return index >= 0 && static_cast<size_t>(index) < wrecked.size() &&
               wrecked[static_cast<size_t>(index)] != 0;
    }
    // Records the wreck. Returns true the FIRST time an instance is knocked over, false if it
    // was already down - the original tests bit 0x40 exactly the same way before acting.
    bool wreck(int index, int32_t tick) {
        if (index < 0 || static_cast<size_t>(index) >= wrecked.size()) return false;
        if (wrecked[static_cast<size_t>(index)]) return false;
        wrecked[static_cast<size_t>(index)] = 1;
        // A full list means no animation: the object appears already settled, from tick one.
        wreck_tick[static_cast<size_t>(index)] =
            (animating < kMaxAnimating) ? tick : (tick - 0x10000);
        if (animating < kMaxAnimating) ++animating;
        ++revision;
        return true;
    }
};

// One piece of DRIVABLE NON-TERRAIN geometry: a ramp, a bridge deck, a tunnel floor, a raised
// platform. NOT scenery you bump into - across the whole game only 181 instances carry one of
// these categories, and they are all things you drive ON.
//
// The footprint is a swept trapezoid in the object's own frame, and the surface over it is a
// plane tilted along local Z. Every field comes from the shape's `.FT`.
struct SimObject {
    int32_t x = 0, y = 0, z = 0;   // world units, game frame (+Y down)
    int32_t yaw_bam16 = 0;
    int32_t x_low = 0, x_high = 0;     // .FT 0x20 / 0x22 - footprint bounds in local X
    int32_t depth_low = 0, depth_high = 0; // .FT 0x24 / 0x26 - half-depth at each X bound
    int32_t height = 0, slope = 0;     // .FT 0x2c / 0x2e - surface height at centre, and its
                                        // rise from the centre line to the footprint edge
    // .FT 0x28 / 0x2a - the SECOND surface of the same profile, in the same linear form. It is
    // the roof over the drivable one: the original flags a vehicle whose centre sits between
    // the two, which is how the engine knows it is inside a tunnel or under a bridge deck.
    int32_t roof_height = 0, roof_slope = 0;
    uint8_t flags = 0;                 // .FT 0x30; bit 0x20 tapers the footprint instead
    bool wide = false;                 // categories 131/132/250/251 skip the local-X bounds test
    bool platform = false;             // categories 52/246: a floor a fixed height over the terrain
    // ⚠️ A TUNNEL IS NEVER A FLOOR - categories 0x13/0x14/0x1b/0x1c, raw 0x83/0x84/0xfa/0xfb,
    // the `ITITUNN*` and `SNOTUNN*` shapes. The original's contact cases 0x13/0x14/0x1b/0x1c are
    // the ONLY drivable-looking categories that never reach the queue the ground
    // probe walks: they handle themselves, on the spot, and only when the roof test says the
    // body is BETWEEN the shape's two surfaces - that is, inside the bore. A tunnel is a hole
    // through a hill, so its floor is the TERRAIN; what the shape contributes is the roof and
    // the two side walls.
    //
    // Treating one as a floor is what made the hill above it intangible: the ground probe
    // stops consulting the terrain the moment an object claims a corner, and a tunnel's deck
    // is hundreds of units below the hilltop. Driving over a tunnel sank the car to the tunnel
    // floor (user, 2026-09-21). See VehicleSim::hit_tunnel.
    bool tunnel = false;
    int32_t platform_lift = 0;         // .FT 0x20, in world units, for that path
    // .FT 0x0c - the shape's bounding radius. This is what the CANDIDATE query tests, and it is
    // tested against the vehicle's CENTRE, not against each corner: see ObjectCandidates.
    int32_t radius = 0;

    // --- the BLOCKING form: everything you cannot drive through --------------------------
    //
    // ⚠️ THE `.FT` IS A UNION. The fields above read 0x1e-0x31 as a drivable surface profile;
    // the fields below read 0x14-0x53 and 0x56-0x75 as a silhouette. Those byte ranges OVERLAP.
    // A shape's collision category picks which reading applies, and no shape is ever both - 181
    // instances in the whole game are drivable, ~8000 are blocking.
    bool blocker = false;
    // THE SLAB family - the original's contact cases 2, 7, 0xf, 0x15, 0x21 (DIRT1's BAY_ROOF,
    // WATER1's PLAT1, BRIDGE, TUBE). The same silhouette/box test as any blocker, then the
    // vertical logic of the car-to-car case: coming from UNDER it, the body is put back where it
    // was; from ABOVE, the same, which makes it a FLOOR; from the SIDE, a wall. See
    // VehicleSim::hit_scenery.
    bool slab = false;
    // The original's contact cases 0x12 and 0x1a (raw 0x81 and 0xf9) - RIG1's WOODBRIJ: a deck the car
    // can drive on and pass UNDER. See VehicleSim::bridge_role.
    bool bridge = false;
    // .FT 0x32: added to the deck by the original when the point is past the footprint's end
    // in local Z (the surface test's flag 2).
    int32_t past_z_lift = 0;
    // ⚠️ THE RAMP FAMILY IS BOTH AT ONCE - categories 0x11 and 0x19, raw 0x80 and 0xf8.
    // The original's cases 0x11/0x19 do not pick a role from the category; they decide it every
    // tick from ONE height test taken at the body's own position (the start-of-tick snapshot):
    //
    //     deck = the profile's drivable surface at the body's XZ, in world Y
    //     body_y >= deck  ->  the body is UNDER the deck. The ramp is a WALL, and the response
    //                         is the ordinary blocking one: sound 0x1c, then the response
    //                         pair - reflection, push-out and damage.
    //     body_y <  deck  ->  the body is ON it. The ramp is a FLOOR, queued as drivable.
    //
    // (+Y points DOWN, so "greater Y" is "lower".) Treating a ramp as a floor and nothing else
    // is what let a jump taken from its tall end LIFT the car to the top instead of stopping
    // it: the wedge's narrow phase resolves a buried point by pushing it straight out to the
    // surface, and that push has no upper bound. Measured on WATER1 before the gate existed:
    // 723 units in a single tick, with the speed going 25788 -> 26078 - the car did not even
    // slow down (user, 2026-09-21: "le vehicule est teleporte tout en haut de la rampe").
    // The bench's RAMPS block is that measurement.
    bool ramp_gate = false;
    // Categories 4, 5 and 9 (raw 0x04/0x05/0x28): contact does not push the car back, it SETS
    // BIT 0x40 of the instance's variant byte - the "this object is wrecked" flag - and plays a
    // sound. That is the destruction trigger the destructibles spec was missing.
    bool destructible = false;
    // A plain OBSTACLE - tree, wall, fence, post, fixed sign (category_blocks): neither a
    // destructible, a ramp nor a slab. What a respawn spot must keep clear of - see
    // SimTerrain::spot_blocked.
    bool hard = false;
    // `.FT` 0x16 read as a u16 >= 0xfffd is a SENTINEL: this shape is not a silhouette but an
    // ORIENTED BOX, whose four bounds follow at 0x18-0x1e. It is not a rare case - 4244 of the
    // ~8000 blocking instances use it, every FENCE, BARRIER and BILBORD among them. A long thin
    // wall cannot be described by 32 radii from its centre, which is presumably why it exists.
    bool use_box = false;
    int32_t box_xmin = 0, box_xmax = 0, box_zmin = 0, box_zmax = 0; // .FT 0x18/0x1a/0x1c/0x1e
    int32_t sector_reach[32] = {};   // .FT 0x14..0x53, object frame
    uint8_t sector_normal[32] = {};  // .FT 0x56..0x75, BAM8
    // .FT 0x04 / 0x06 - the slab of Y the object occupies. +Y is DOWN, so y_bottom > y_top.
    int32_t y_bottom = 0, y_top = 0;
    // Index back into the level's instance list, so a destroyed object can be marked. -1 when
    // the object came from somewhere without one.
    int instance_index = -1;
    // MAP.DAT's offset-21 byte (SceneryInstance::aux_byte), which on a DESTRUCTIBLE category is
    // the "var" the vehicle scripts talk about: it picks which impact sound the prop makes.
    // CONFIRMED in the original's contact cases 4 and 5 - see hit_sound_index().
    uint8_t splat_var = 0;
};

// What one contact produced. The original keeps these in globals; they are per-contact, so they
// are a value type here.
struct ObjectContact {
    bool hit = false;
    int32_t depth = 0;      // how far the vehicle is inside the object
    int32_t normal_bam16 = 0; // the direction OUT of the object at the contact, world frame
    int32_t bearing = 0;    // the contact bearing, BAM16, vehicle -> object
    const SimObject* object = nullptr;
    // The separation the test compared the distance against, filled even when there is no hit.
    // This is what the bench checks its own expectation against: computing the expected reach a
    // second, slightly different way is how three yardstick bugs in a row got mistaken for
    // physics bugs.
    int32_t reach = 0;
};

// The candidate list the original rebuilds once per tick, and then walks for
// every corner. Two things about it decide how a ramp feels, and neither is a detail:
//
//   * it is gated on the VEHICLE'S CENTRE, not on each corner - an object stays in play while
//     the body is within `r_vehicle + r_object` of it. So a ramp's surface runs
//     on for about half a vehicle length past its own footprint, and the front corners do not
//     drop onto the terrain far below the instant they pass the lip;
//   * it holds at most FOUR entries, and that count is what the original's surface test
//     reads to decide whether to test an object's FAR edge at all.
//
// Order: the original's spatial query walks the map grid, this walks the level's instance list.
// Both are deterministic; they only differ in WHICH four survive where more than four overlap.
struct ObjectCandidates {
    const SimObject* items[4] = {nullptr, nullptr, nullptr, nullptr};
    int count = 0;
    // The original roof test's own by-product: the body centre is between an object's roof and its
    // drivable surface. The physics does not read it; the CAMERA does - see ChaseCamera.
    bool inside = false;
    // The original tests the far-Z bound of a footprint only when the list holds 2 or more.
    bool test_far() const { return count >= 2; }
};

// The terrain, as the physics sees it. Points at the level's own MAP.MPH / MAP.MPC / MAP.CHT
// bytes - no copy, no conversion.
struct SimTerrain {
    const uint8_t* mph = nullptr;  // heightmap, width*height bytes
    const uint8_t* mpc = nullptr;  // ground map; the CHT-mapped high nibble is the land type
    const uint8_t* cht = nullptr;  // 768 bytes: [0..255] and [256..511] are the two remap
                                    // tables the engine picks between by cell triangle
    int w = 256, h = 256;
    // The level script's `land` line: 16 entries, high-nibble -> surface type 0-7.
    uint8_t land[16] = {};
    // Ramps, bridges, tunnel floors and platforms. Small - 181 in the entire game - so every
    // one is tested against every corner, which is what the original's own "nearby" list
    // amounts to in practice.
    std::vector<SimObject> objects;

    // Everything you BUMP INTO: trees, fences, walls, posts, billboards, and the destructible
    // props. ~8000 instances against the 181 drivable ones, so this one is worth a broad phase.
    // Kept apart from `objects` on purpose - the two read the same `.FT` bytes with different
    // meanings (see SimObject), and mixing them in one list is how that union turns into a bug.
    std::vector<SimObject> blockers;
    // `blockers` sorted into a coarse grid, so a tick tests a handful of objects and not 8000.
    // Cell size is kGridCell world units; `grid[c]` holds indices into `blockers`. This replaces
    // the original's own two-level query (over MAP.GRP's group boxes), which is a
    // level-authoring structure we do not need to reproduce to get the same answers - what has
    // to match is WHICH objects end up tested, and a grid finer than the largest object's reach
    // gives exactly that set.
    //
    // SIZED ON THE MAP, never less than kGridDim: 64 x 2048 = 131072 = a shipped 256-texel map
    // exactly, so the shipped levels see the grid they always had. A bigger map (a generated
    // circuit) gets a bigger grid; it used to be fixed at 64 and silently dropped whatever lay
    // past it, which a car then drove through (code review D2). An object still outside the
    // grid - off the map's edge - is counted and said once when the grid is built.
    static constexpr int32_t kGridCell = 2048;
    static constexpr int kGridDim = 64;
    int grid_w = 0, grid_h = 0;   // cells, set by build_blocker_grid()
    std::vector<std::vector<int32_t>> grid;
    void build_blocker_grid();
    // The cell (gx, gz), or null outside the grid.
    const std::vector<int32_t>* grid_cell(int gx, int gz) const {
        if (gx < 0 || gz < 0 || gx >= grid_w || gz >= grid_h) return nullptr;
        return &grid[static_cast<size_t>(gz) * static_cast<size_t>(grid_w) + static_cast<size_t>(gx)];
    }

    bool valid() const { return mph && mpc && w > 0 && h > 0; }

    // BENCH ONLY: terrain_probe as the original computes it, 16-bit narrowing and unbounded
    // vertical push included - i.e. WITHOUT deliberate deviation 4. The game never sets it;
    // physics_bench flips it to measure what the deviation costs (SAUT block).
    bool faithful_probe = false;
    // The deviations' switches and values used to live here; they are RULES, not ground, and
    // are in RaceRules now (sim/race_rules.h), handed to each car by its session.

    // Terrain height under a fixed-point world position, in the same 1/512 fixed point, +Y down
    // (so higher ground is MORE NEGATIVE). Triangle-interpolated inside the cell exactly like
    // the original's height routine - which is also where the 16 world units per height byte
    // come from.
    //
    // `slope_x` / `slope_z` optionally receive the two cell-edge slope angles of the triangle
    // the point falls in, in BAM16, positive where the ground rises toward +X / +Z. They are
    // the same pair the original returns alongside the height, and the push-back needs them to
    // slide a penetrating point along the surface instead of straight up.
    int32_t height_fx(int32_t x_fx, int32_t z_fx,
                      int32_t* slope_x = nullptr, int32_t* slope_z = nullptr) const;

    // The original's terrain probe, the whole of it, for ONE point. `ground_fx` always receives the terrain
    // height there. Returns -1 and fills `push` (same 1/512 fixed point) when the point is
    // UNDER the terrain, 0 when it is above it.
    //
    // It is a method rather than inline code in the corner loop because the original calls it
    // from two places: the terrain path, and the `platform` path of the corner probe, which probes
    // the terrain from a point pushed DOWN by the object's lift and then raises the answer back
    // up. That shared call is what gives a low platform - a WATER1 root - a real oblique push
    // instead of a flat vertical one.
    int terrain_probe(int32_t x_fx, int32_t y_fx, int32_t z_fx, int32_t& ground_fx,
                      int32_t push[3]) const;

    // Surface type 0-7 at a position: MAP.MPC through the CHT table for this triangle half,
    // then the level's own `land` line, as in the original.
    int land_type(int32_t x_fx, int32_t z_fx) const;

    // The original's spatial query: which objects are in play for a body centred here, this
    // tick. Call it ONCE per tick and hand the result to every corner's object_probe.
    //
    // `body` + `yaw_bam16`, when given, add the narrow phase the original runs before ANY of its
    // cases: a TUNNEL shape only takes part if the body's rectangle touches the `.FT` box. The
    // bounding circle alone is far too generous for a tube thousands of units long - see
    // touches_box.
    //
    // A BRIDGE (cases 0x12/0x1a) is a floor only when VehicleSim::hit_bridges said so this tick:
    // `bridges_ok` lists those, and a bridge absent from it is not a candidate at all.
    ObjectCandidates gather_candidates(int32_t x_fx, int32_t y_fx, int32_t z_fx,
                                       int32_t body_radius, const VehicleBox* body = nullptr,
                                       int32_t yaw_bam16 = 0,
                                       const SimObject* const* bridges_ok = nullptr,
                                       int bridges_ok_count = 0) const;

    // The original's contact gate, the part of it that matters for a tunnel: does the body's rectangle,
    // centred at world (x, z) and turned by `yaw_bam16`, overlap the object's `.FT` box? True
    // for an object that has no box.
    //
    // ⚠️ Without it a tunnel acts FAR outside its own footprint. Two reports of 2026-09-23, one
    // cause: CITY4's two parallel bores - a car in the left one was inside the right one's
    // bounding circle, so the right one's side wall pushed it the whole way across ("téléporté
    // dans celui de droite"); and DIRT3's roofless mouth apron, a floor above its deck with no
    // lateral bound, reached across the hill at deck height ("si on passe dessus, on tombe
    // dedans").
    static bool touches_box(const SimObject& o, int32_t x, int32_t z, int32_t yaw_bam16,
                            const VehicleBox& body);

    // The drivable-object probe, as the original's. Returns 1 when the point rests on
    // an object's surface, -1 when it is inside one (and fills `push`), 0 when no object
    // applies. On 1 or -1, `ground_out` is the object's surface instead of the terrain.
    // `who`, when given, receives the object that decided the result - diagnostics only.
    int object_probe(const ObjectCandidates& cand, int32_t x_fx, int32_t y_fx, int32_t z_fx,
                     int32_t& ground_out, int32_t push[3],
                     const SimObject** who = nullptr) const;

    // The blocking narrow phase, the original's 32-sector silhouette and oriented box tests,
    // against one object. `yaw` and the reach table make it direction-dependent, so the
    // vehicle's own profile has to be passed in.
    ObjectContact blocker_contact(const SimObject& o, int32_t x, int32_t y, int32_t z,
                                  int32_t yaw_bam16, int32_t ground,
                                  const VehicleBox& body, bool vertical_gate = true) const;

    // The scenery half of the original's respawn-spot check. Would a body of `body`, set down at
    // world (x, z) facing `yaw_bam16`, touch an obstacle? The same two outline tests as the
    // driving contact (the box and the silhouette) and NO vertical test:
    // the car is about to drop from 500 units up. Only `hard` objects count - see there.
    bool spot_blocked(int32_t x, int32_t z, int32_t yaw_bam16, const VehicleBox& body) const;

    // Every blocking object in reach of a body centred here, in a deterministic order.
    //
    // ⚠️ ALL of them, not just the deepest. The original's query invokes the response callback once per
    // overlapping object and each call does its own push, so a car wedged between two of them
    // gets both. Keeping only the deepest looks equivalent and is not: on a ROW of barriers,
    // driving at the end of one means brushing the flank of its neighbour, the flank contact is
    // deeper, and the car gets shoved sideways along the row instead of being stopped by the end
    // it actually hit. Measured on CITY1, where it let the car reach 62 units from the centre of
    // a barrier that should hold it at 297.
    int blocker_probe(int32_t x, int32_t y, int32_t z, int32_t yaw_bam16, int32_t ground,
                      const VehicleBox& body, ObjectContact* out, int max_out) const;
};

// Picks the BLOCKING scenery out of a level - everything a car bumps into - and reads each
// shape's silhouette from its `.FT`. ~8000 instances of the ~8760 in the game, against the 181
// `build_sim_objects` keeps. Hand the result to SimTerrain::blockers and call build_blocker_grid.
std::vector<SimObject> build_sim_blockers(
    const std::vector<SceneryInstance>& instances,
    const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup);

// Picks the drivable objects out of a level's scenery and reads each one's collision profile
// from its `.FT`. `ft_lookup` resolves a shape name (no extension) to that shape's `.FT` bytes.
//
// Only 181 instances in the entire game qualify: every ramp, bridge deck, tunnel floor and
// raised platform. Everything else - trees, fences, signs - is not collidable at all in the
// original, which is why you can mow a line of cones down.
std::vector<SimObject> build_sim_objects(
    const std::vector<SceneryInstance>& instances,
    const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup);

// The six logical actions. A modern input layer produces these; they are the whole control
// surface of a vehicle.
struct VehicleInput {
    bool accel = false, brake = false, left = false, right = false;
    bool nitro = false, recover = false;
};

// Where a `recover` puts the vehicle back. One entry is enough to drive the key; the real
// checkpoint table replaces this when the race system lands.
struct RespawnPoint {
    int32_t x = 0, z = 0;     // world units
    int32_t yaw_bam16 = 0;
};

// How far a rotor or prop turns in one 28 ms tick, BAM16. HYPOTHESE: chosen so a rotor makes a
// full turn in 224 ms, roughly what a real main rotor does; the original own rate has not been
// found. It MUST stay below 0x8000, or an interpolated display reads the turn backwards.
constexpr int32_t kRotorSpinPerTick = 0x2000;

class VehicleSim {
public:
    // --- simulation state, named after what it is -----------------------------------------
    int32_t pitch = 0, yaw = 0, roll = 0;             // BAM24 signed
    int32_t pitch_rate = 0, yaw_rate = 0, roll_rate = 0; // BAM24 per tick
    int32_t speed = 0;                                 // clamped to +/-0xFFFA
    int32_t travel = 0;                                // BAM16
    int32_t vy = 0;                                    // + = falling
    int32_t vfwd = 0, vlat = 0;                        // clamped +/-0xB4FA
    int32_t px = 0, py = 0, pz = 0;                    // 1/512 world units

    int32_t corner_x[8] = {}, corner_y[8] = {}, corner_z[8] = {}; // world units
    int32_t corner_ground[8] = {};                                // world units
    uint16_t contact = 0, contact_prev = 0;            // this tick and the last
    uint16_t near_ground = 0, near_ground_prev = 0;    // this tick and the last
    int land = 0;                                      // surface type 0-7
    int32_t terrain_pitch = 0, terrain_roll = 0;       // BAM16
    int32_t height_above = 0;                          // height over the ground

    int32_t thrust = 0;                                // the gear curve's output
    int32_t accel_delta = 0;                           // this tick's change in vfwd
    int32_t steer_target = 0;                          // the yaw-rate the wheels ask for
    int32_t gear = 0, rev = 0;                         // written by the gear curve

    int32_t steer_axis = 0, throttle_axis = 0;         // +/-256
    int32_t lift_axis = 0;                             // aircraft collective
    int32_t throttle_fwd = 0, throttle_rev = 0;        // 0..256
    bool mode = false;                                 // see the spec, this is subtle
    // THE SKID FLAG, and it is NOT `mode`. CONFIRMÉ 2026-09-21 at the end of the original's
    // grip routine, which stores three scratch bytes into the instance at once:
    //
    //     mode     = scratch byte 3   (the `mode` accumulator)
    //     skidding = scratch byte 2   <- this
    //     moving   = a second scratch (|vfwd| > 3000, "actually moving")
    //
    // Byte 2 is written in exactly ONE place in the whole tick, inside the GROUNDED branch:
    // `if (vlat < -5000 || 5000 < vlat)`. So skidding is a plain threshold on the LATERAL
    // velocity, nothing more - and a vehicle in the air never sets it.
    //
    // ⚠️ Do not confuse it with `mode`. `mode` is fed by `yavel`, which converges whenever the
    // yaw rate is chasing the steering input - that is to say whenever the car TURNS. Driving
    // the particle emitter off `mode` made dust fly on every corner, which is how this got
    // found (user, 2026-09-21: "on dirait qu'elles s'affichent dès que le véhicule tourne et pas
    // quand il glisse").
    //
    // This is also what should trigger the `hnd fx skid <surface>` sound.
    bool skidding = false;
    bool moving = false;                               // |vfwd| > 3000
    uint8_t input_bits = 0;                            // this tick's actions
    bool may_accel = true, may_reverse = true;         // direction guards
    int32_t nitro_fuel = 0;                            // the boost reserve
    int32_t boost_timer = 0;                           // ticks of boost left
    bool prev_nitro_key = false;                       // last tick's boost key
    // The boost key pressed with an empty reserve. The original plays an fx entry on exactly
    // this case - the script calls the sample `notturb` - so it is an EVENT the audio needs,
    // and it lives here because only the tick can see it. Physics reads it never.
    bool nitro_refused = false;
    // The DOWNWARD speed at the instant the body touched down again, or 0 on any other tick.
    // The original gates its landing sound on it: it plays `hnd fx land <surface>`
    // only when `impact / 2 > 3999`, and passes `impact / 2 - 4000` as the sound's VOLUME - so a
    // gentle touchdown is quiet and only a real drop is loud. Audio reads it; physics does not.
    int32_t landing_impact = 0;
    // How many overlapping objects one tick applies a push for. Four is the original's own
    // candidate cap; eight leaves room where our grid finds more than it does.
    static constexpr int kMaxSceneryContacts = 8;
    // The original gives a destructible prop NO push: you knock it over and drive through it.
    // ⚠️ That IS right, and the user settled it on 2026-09-16 by naming the two kinds of sign
    // apart: the ones ON A WOODEN POST (ROADSIGN, MSIGN1/2 - categories 4/5/9) are the ones you
    // are meant to flatten, and the ones WITHOUT a post plus the billboards (DIR1, BILBORD1/2/3 -
    // categories 0/31/36) are the ones that must stop you. The data already splits them exactly
    // that way. So a car going through a sign is a bug in the BLOCKING path, never a reason to
    // make destructibles solid - an earlier guess in that direction was wrong and is reverted.
    static constexpr bool kDestructiblesPushBack = false;

    // --- scenery contact, this tick ------------------------------------------------------
    bool scenery_hit = false;         // what drives the camera shake
    // The cabin head dip this tick: `vfwd >> 4` when `scenery_hit`, else 0 (the tail of the
    // original's collision pass). ⚠️ Taken INSIDE the collision pass, where `vfwd` still holds
    // LAST tick's forward
    // speed - the contact response only rewrites speed/travel, and vfwd is re-derived later in
    // the tick. Reading `vfwd` after step() gets the reflected speed instead: negative, halved,
    // and the head went UP by a few degrees rather than down by ten (user, 2026-09-29: "la
    // caméra lors du choc ne bouge pas"). View-only: nothing in the simulation reads it.
    int32_t head_kick = 0;

    // --- THE OTHER RACERS --------------------------------------------------------------------
    // The original runs the scenery query, then the car-against-car test against each of the
    // five other cars. The race
    // session points `field` at every racer, in grid order, this one included at `field_self`;
    // a car on its own (the bench, the viewer) leaves it null and simply meets nobody.
    //
    // ⚠️ DETERMINISM: the cars are stepped one after the other in grid order, so car i sees
    // cars 0..i-1 where they are AFTER this tick and cars i+1.. where they were BEFORE it -
    // exactly what the original's sequential loop does. Never step them in any other order.
    // Not const: in a two-body mode the contact pushes the OTHER car too (below).
    VehicleSim* const* field = nullptr;
    int field_count = 0;
    int field_self = -1;
    // THE TWO-BODY CONTACT - the car-against-car test in the original's game modes 0x20 and
    // 0x23. In a normal race the velocity exchange changes only this car and each does its own
    // half of the contact when its turn comes; in a two-body mode the other car gets the
    // mirrored velocity change as well, and the push-out moves EACH car half the separation
    // apart. 0x20 does it all locally; 0x23, the original's network mode, pushes only itself
    // and queues the other's reaction for its owner. Every machine of OUR lockstep simulates every
    // car, so 0x20 is exactly what 0x23 converges to - set by the session for a race between
    // humans. A GHOST never takes the reaction: its pose is the recording's.
    bool two_body_contacts = false;
    bool replayed = false;            // a ghost: an obstacle, never pushed
    bool active = true;               // a car out of the race is not an obstacle
    // The racer touched this tick, or -1 - an EVENT, for the audio: the original plays fx 0x1e
    // against a car of the same group and 0x1f against any other.
    int vehicle_hit = -1;
    // The car-against-car SOUND, an event: the racer whose `hit 2`/`hit 3` is to be played this
    // tick, or -1. The original queues it on the OTHER car (fx 0x1e/0x1f),
    // and only when that car is not what this one touched last - as read from the
    // snapshot the original takes at the top of the tick, the last thing
    // touched. So a car pressed against another bangs once, not every tick.
    int vehicle_hit_sound = -1;
    // The "last touched" field for a car: the racer touched last, -1 once anything else has been
    // touched since.
    int last_contact_car = -1;
    // The body's world Y at the START of the tick, before the integration - from the
    // whole-instance snapshot the original copies at the top of the tick.
    // The original restores it when one car lands on another's roof.
    int32_t snap_y = 0;
    int32_t snap_x = 0, snap_z = 0;   // the rest of the same snapshot - the body centre, world units
    int scenery_hit_instance = -1;    // the "last touched" field, as an index into the level's instance list
    // Every instance touched THIS tick, not just the last one. A car wedged in a corner touches
    // two or three at once, and "did I touch that particular object" is the only unambiguous
    // question to ask of a contact - see the bench's SCENERY block.
    int scenery_hit_instances[8] = {};
    int scenery_hit_count = 0;
    bool touched_instance(int idx) const {
        for (int i = 0; i < scenery_hit_count; ++i)
            if (scenery_hit_instances[i] == idx) return true;
        return false;
    }
    // The instance a destructible object contact knocked over ON THIS TICK, or -1. The state
    // that LASTS is `world`'s, because it is shared and permanent.
    //
    // ⚠️ IT IS AN EVENT, NOT A LATCH, and it is cleared at the top of every tick. It used not to
    // be, and the comment here already claimed "one tick at a time" while the code never reset
    // it - so once you had flattened one sheep the field stayed set for ever. The audio read it
    // as "a prop is being crushed right now" and played the splat sample THIRTY-SIX TIMES A
    // SECOND, for the rest of the race, even stopped (user, 2026-09-21: "le son des moutons
    // tourne à l'infini et finit par saturer"). Anything that reads this must see an edge.
    int scenery_wrecked = -1;
    // The `splat_var` of that prop, so the audio can tell a sheep from a cone. Meaningless when
    // `scenery_wrecked` is -1.
    uint8_t scenery_wrecked_var = 0;
    int32_t impact_magnitude = 0;     // this tick's impact
    // Where the impact SPARKS are to be born, and the one-shot flag that consumes it - the
    // original's per-vehicle fields, written by its contact response and cleared by the
    // spot emitter. See sim/particles.h.
    //
    // ⚠️ ÉCART ASSUMÉ on the POINT. The original stores the collision routine's own contact
    // point (in globals); our blocker test does not keep one, so this is
    // the car's centre carried forward to its bumper along the contact bearing. That is within
    // half a car length of the truth and on the right side of the car, which is what a spark
    // needs; nothing in the simulation reads it back.
    int32_t spark_x = 0, spark_y = 0, spark_z = 0;
    bool spark_pending = false;
    // Raw damage and the 0-100 percentage the original derives from it.
    // A NEGATIVE `damage` is the original's "this vehicle is destroyed" sentinel.
    int32_t damage = 0;
    int32_t damage_percent = 0;
    // How much punishment this vehicle can take, in the same units as `damage`. NOT simply the
    // script's `hnd damage`: the race setup scales it by the game's damage option, and
    // the original does exactly `capacity = hnd_damage * (100 - option) / 100`, floored at 1.
    // Computed in reset(), and again by set_damage_option().
    int32_t damage_capacity = 0;
    // The game's damage option, 0..100. ⚠️ IT IS THE PERCENTAGE OF DAMAGE THE CAR SUFFERS on an
    // impact, so it runs the way a player expects: **100 means taking it all, and the car breaks
    // on the first real crash; 30 means taking less, and it survives several.** Higher = more
    // fragile, which is why the formula above SUBTRACTS it.
    //
    // ⚠️ The shipped `.CFG` files declare no `damage` line at all, so the default lives in the
    // executable's static data. The value is the user's, who
    // owns the game: **30 by default**, leaving a gauge of 70 for a vehicle that declares
    // `hnd damage 100`. One number, and it is the whole difficulty of the damage model.
    // ⚠️ CONFIRMED, 2026-09-21, and no longer only the user's word: `damage %i,%i,%i` is a
    // BIGRED.CFG line that no shipped `.CFG` declares, so the value is the executable's static
    // one: damage is ON by default, and the option is **30**. The user had it exactly right.
    static constexpr int32_t kDefaultDamageOption = 30;
    int32_t damage_option = kDefaultDamageOption;
    // ⚠️ ÉCART ASSUMÉ, and the only one in the damage model. The original's contact response does
    // `damage += magnitude >> 8`, full stop, and the same three lines appear in its two other
    // impact routines. With the gauge above that is
    // brutal beyond playing: `magnitude` IS the speed going into the surface, a car tops out
    // near 30 000, so one head-on impact adds ~117 against a capacity of 70. **The car is
    // destroyed by the first real crash, every time.**
    //
    // The user, who owns the game: "in the original, hitting something head-on at full speed
    // does barely 10% damage". Ten hits, not one. Nothing in the original's arithmetic produces
    // that - not the option (which can only shrink the gauge), not `hnd damage` (100 to 150
    // across the roster) - so the shift is where the deviation goes, alone and named.
    //
    // 12 instead of 8 is one sixteenth of the original rate: a full-speed frontal impact costs
    // 8 points of a 70-point gauge, about 11%, and it takes nine of them to wreck the car.
    // Set it back to 8 for the original behaviour.
    static constexpr int32_t kImpactDamageShift = 12;
    static constexpr int32_t kOriginalImpactDamageShift = 8;
    // The tick the vehicle was destroyed on - an EVENT, like the one above, so the explosion is
    // played once and not for ever. `hnd fx hit 7` is `explode` on every vehicle in the game.
    bool just_destroyed = false;
    bool boost_active = false;                         // drives the flame
    uint8_t flame_phase = 0;                           // the flame's flipbook frame
    // Visual, but advanced per TICK so it stays frame-rate independent and deterministic like
    // everything else here: how far the wheels have rolled, as a BAM16 angle.
    int32_t wheel_spin = 0;
    // The same, for the powered parts: rotors and props. ⚠️ HYPOTHÈSE - the RATE is ours, not
    // the original's. What is not a guess is that it has to live
    // here, on the tick, rather than on a frame counter.
    int32_t rotor_spin = 0;
    // Set by the original's roof test and cleared by its collision pass every tick: the body is between
    // a drivable object's roof and its floor - a tunnel, an underpass, a bridge soffit. Nothing
    // in the handling reads it; the camera reads it three times over (see ChaseCamera).
    bool in_tunnel = false;
    bool upside_down = false;                          // flips on the PITCH fold
    // Written by the original's angle normalisation BEFORE it folds the pitch: the roll is past
    // +/-90 degrees. Not the same thing as `upside_down`: that one flips on the PITCH fold. The
    // lean of the `tilter z` is gated on this one.
    bool rolled_over = false;
    // DIAGNOSTICS ONLY - never read by the simulation, never in the checksum. The corner whose
    // push-back lifted the body the most this tick: where it came from (0 none, 1 terrain,
    // 2 drivable object, 3 platform) and, for an object, which one. physics_bench's LIFT block.
    int push_source = 0;
    // DIAGNOSTICS ONLY: the `tilter x` took over the pitch this tick.
    bool tilter_x_fired = false;
    const SimObject* push_object = nullptr;
    // DIAGNOSTICS: this tick's resolved push-back (the private push_x/y/z), 1/512 units.
    int32_t push_xyz(int axis) const { return axis == 0 ? push_x : (axis == 1 ? push_y : push_z); }
    int32_t push_worst_y = 0;   // world units
    int32_t push_x_dbg() const { return push_x; }
    int32_t push_y_dbg() const { return push_y; }
    int32_t push_z_dbg() const { return push_z; }
    int32_t air_ticks = 0;                             // ticks in the air
    bool enabled = true;                               // damage >= 0
    // Bumped by every recover(). The original re-enters camera mode 4 at the same point
    // (its recover sets camera mode 3 then 4), which re-plants the camera; watching
    // this counter is how the renderer knows a teleport happened rather than a movement.
    uint32_t respawns = 0;

    const VehicleTuning* tune = nullptr;
    const SimTerrain* terrain = nullptr;
    // The race's rules (sim/race_rules.h) - the session's, re-pointed every tick
    // (RaceSession::link_field). Null: the defaults, which is what a car on a bench runs.
    const RaceRules* rules = nullptr;
    const RaceRules& race_rules() const {
        static const RaceRules kDefaults;
        return rules ? *rules : kDefaults;
    }
    // The shared destruction state, or null when nothing is watching (the bench's narrow-phase
    // blocks). Every vehicle in a race points at the SAME one.
    SceneryState* world = nullptr;
    // Ticks elapsed since the race started, advanced by step(). The destruction flipbook reads
    // it, and it is what a wreck's instant is expressed in.
    int32_t tick = 0;
    // Ticks since the car was destroyed, 0 while it runs - the original's damage field counting down
    // from -1. The physics never reads it; the drawing does (the explosion's frames), and so do
    // the race's end rules (RaceScreen::wrecked_for) - which is why it must stay in the resync
    // snapshot with the rest of the car.
    int32_t wrecked_ticks = 0;
    RespawnPoint respawn;

    // `nitros` is the level's own `nitros` line. WHICH of its two numbers becomes the reserve
    // depends on the vehicle's boost mode - see the note in reset().
    void reset(const VehicleTuning& t, const SimTerrain& ter, const RespawnPoint& start,
               const NitroReserve& nitros = {});
    // ... under rules of its own, which must outlive the car (a bench's).
    void reset(const VehicleTuning& t, const SimTerrain& ter, const RaceRules& r,
               const RespawnPoint& start, const NitroReserve& nitros = {});

    // One tick of kPhysicsTickMs (28 ms). Call it a whole number of times per wall-clock
    // second, never once per rendered frame.
    void step(const VehicleInput& in);

    // Puts the vehicle back on its respawn point, `drop` units up, all velocity cleared -
    // as the original's recover does, which uses 500. A grid start goes through place_on_grid() instead.
    // Sets the game's damage option (0..100) and recomputes `damage_capacity`. `reset()` calls
    // it with whatever is currently set, so changing it means calling this afterwards.
    void set_damage_option(int32_t option);
    void recover(int32_t drop = 500);
    // The half of the original's respawn-spot check that looks at the other cars: would a body
    // of `t`, set down at world (x, z) facing `yaw_bam16`, touch THIS car? The box broad phase
    // and the two silhouettes, and no vertical test - the respawn drops from 500 up, so
    // the original does not ask. False for a car out of the race.
    bool blocks_spot(const VehicleTuning& t, int32_t x, int32_t z, int32_t yaw_bam16) const;
    // The start on the grid, as the original's grid setup ends: the body's ORIGIN goes `.FT` +4 (the
    // box's `ybottom`) above the terrain, so the underside of the box rests on the ground.
    // ⚠️ Not `recover(0)`: that puts the origin ON the ground and sinks the lower half of the
    // body into it - and the physics is frozen through the countdown, so nothing lifts it out
    // before the green (the 2026-09-23 "enfoncé dans le sol" report).
    void place_on_grid() { recover(tune ? tune->box.ybottom : 0); }

    // A GHOST's dressing, after GhostDriver::tick has written its pose (sim/replay.h). The
    // recording carries only the pose; what the DISPLAY reads besides it - the ground under the
    // corners and its surface, the body-frame velocity, the wheels and the rotors - is derived
    // here, so a ghost turns its wheels and raises dust like a driven car. Integer only: a
    // ghost is part of the simulated state and must stay bit-identical on every machine.
    void dress_replayed();

    // --- read-only views for the renderer ---
    int32_t world_x() const { return px >> 9; }
    int32_t world_y() const { return py >> 9; }
    int32_t world_z() const { return pz >> 9; }
    int32_t yaw_bam16() const { return yaw >> 8; }
    int32_t pitch_bam16() const { return pitch >> 8; }
    int32_t roll_bam16() const { return roll >> 8; }
    // Speed as a human number, for a HUD: 1/512 world units per TICK -> world units per
    // second. The divisor is the tick length in milliseconds and nothing else - hard-coding a
    // number here is how a display ends up reporting double the real speed.
    int32_t speed_per_second() const { return speed * 1000 / kPhysicsTickMs / 512; }

private:
    friend struct SnapshotAccess;   // sim/snapshot.cpp - a resync copies everything
    void integrate();
    void sample_ground();
    void apply_impact();
    void read_input(const VehicleInput& in);
    void run_engine();
    void apply_grip();
    void apply_air();
    void apply_tilter();
    void normalise_angles();
    void recompose();
    void update_nitro(const VehicleInput& in);
    // The scenery half of the original's collision pass plus its response pair - see the
    // definition.
    void hit_scenery();
    // The original's tunnel cases: the bore's two side walls and its roof, and never a floor.
    void hit_tunnel();
    // The original's contact cases 0x12/0x1a - a BRIDGE deck: a floor from above, a ceiling from below,
    // a wall from the side at deck level, and nothing at all to a body passing well under it.
    void hit_bridges();
    // This tick's eight box corners in world units, from the current pose.
    void body_corners(int32_t* xs, int32_t* ys, int32_t* zs) const;
    // The bridges hit_bridges() allowed as a FLOOR this tick, for gather_candidates.
    const SimObject* bridge_floor[4] = {nullptr, nullptr, nullptr, nullptr};
    int bridge_floor_count = 0;
    // The original's response pair - reflection, then push-out and damage - that every blocking
    // contact ends in. `normal` points out of the obstacle in the world frame.
    void resolve_contact(int32_t normal, int32_t depth, int32_t bearing);
    // The second half of that pair on its own - the push out along `normal` and the damage.
    // resolve_contact is the reflection followed by this; a car-against-car contact is the
    // velocity exchange followed by it.
    void push_and_damage(int32_t normal, int32_t depth);
    // The tail of the original's impact routines: the damage an impact of `magnitude` does.
    void take_impact_damage(int32_t magnitude);
    // The original's car-against-car test against every other racer - see `field`.
    void hit_vehicles();

    // The rate limiter that is the whole of `skid`, as in the original. Returns true when it is STILL
    // converging, which is what feeds the `mode` flag.
    static bool approach(int32_t& cur, int32_t target, int32_t step);
    bool approach_mode(int32_t& cur, int32_t target, const SkidPair& p) {
        return approach(cur, target, p.step(mode ? 1 : 0));
    }

    // The original's piecewise-linear gear curve. Returns the thrust and writes gear/rev.
    int32_t gear_curve(int32_t input, const std::vector<GearRow>& rows);

    int32_t push_x = 0, push_y = 0, push_z = 0; // this tick's resolved penetration push-back
    bool mode_accum = false;
    // The `skidding` accumulator, zeroed with `mode_accum` at the top of the tick and stored at
    // the end - the original keeps both bytes in the same scratch dword.
    bool skid_accum = false;
    // The two flags of the original's righting step. ⚠️ They are computed BEFORE the righting torque is
    // added and applied AFTER the tilter - the ordering is the whole point, see step().
    bool snap_pitch = false, snap_roll = false;
};

} // namespace bigred
