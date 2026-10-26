// `<VEHICLE>.TXT` - the handling tuning and the attachment sockets of one vehicle type.
//
//
// Two things bite anyone reading this header without the spec:
//   - land type 0 BROADCASTS to all 8 slots. Applied here at parse time, like the original does.
//   - `cogs` is NOT stored in the order it is written. The accessors below are the truth.
#pragma once

#include "game_files.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bigred {

constexpr int kLandTypeCount = 8; // TRACK GRIPPY SLIPPY CLIFFS GRASS SAND RAMPS AIR

// One `hnd skid <key> <land>, low, high` entry: the maximum change per tick, picked by the
// `mode` feedback flag (0 = settled last tick, 1 = still converging). See VehicleSim::approach.
struct SkidPair {
    int32_t low = 0, high = 0;
    int32_t step(int mode) const { return mode == 0 ? low : high; }
};

// One row of a `hnd gear f|b` table. The engine stores these column-major; the column order in
// the file is (speed, thrust, gear, rev_lo, rev_hi).
struct GearRow {
    int32_t speed = 0;   // threshold on the vehicle's own forward speed - the curve's abscissa
    int32_t thrust = 0;  // interpolated
    int32_t gear = 0;    // NOT interpolated: the row's value
    int32_t rev_lo = 0, rev_hi = 0; // 0-255 rev needle / engine pitch, interpolated lo -> hi
};

// `hnd tilter z|x <mode>, a, b[, c]`. Body lean. NOT cosmetic: it drives the real angular rates.
struct TilterAxis {
    int16_t mode = 0;
    int32_t a = 0, b = 0, c = 0;
};

// One `wheel` line: a mesh attachment socket. Wheels, the driver's head, effect emitters.
// ⚠️ NOTHING to do with ground contact - that is the .FT bounding box, see VehicleBox.
struct WheelAttach {
    int32_t flags = 0;      // role. &80 set = not a wheel
    int32_t shape_sel = 0;  // low nibble = index into the section's SHAPE TABLE (see below);
                            // &40 = take from the global table instead
    int32_t pos[3] = {0, 0, 0};  // local, true world units (same scale as the halved .3DW)
    int32_t rot[3] = {0, 0, 0};  // BAM 16
    int32_t size = 0, p9 = 0;
    std::string shape;      // resolved shape name, empty when out of range or global
    bool coloured = false;  // the resolved shape owns the 7 team variants: swap the last
                            // character for the colour digit, exactly like the body

    bool is_wheel() const { return (flags & 0x80) == 0 && flags != 0xFF; }
    // ⚠️ AS IN THE ORIGINAL, and not what the bit layout suggests: the original tests
    // `flags & 0x31`, so a wheel turns with the steering only when bit 4 is set AND bit 5 is
    // clear. Over the 50 wheels of the game that is `&12`/`&13` and their kin - and it means
    // the two SIX-WHEELERS, the ATV and the BUGGY, steer NONE of their wheels visually: all
    // twelve carry `&30`/`&31`, both bits. Reading bit 1 instead, as this did until
    // 2026-09-21, turned four wheels on each of them.
    bool is_steered() const {
        return is_wheel() && ((flags & 0x31) == 0x10 || (flags & 0x31) == 0x11);
    }
    // How far this part yaws per unit of the steering axis (+/-256), in BAM16, straight out of
    // the original's wheel-steering routine:
    //   - a steered wheel takes 15, so +/-3840 BAM = +/-21 degrees at full lock;
    //   - `&80` - the hovercraft's two rudders and the INFLATE's engine - takes MINUS its own
    //     `size` field, which reads 25 on all three, so they swing +/-35 degrees the other way;
    //   - everything else stays put.
    // ⚠️ The original also has `&20`/`&21` branches (a rear axle steering the opposite way, on
    // the right side) that NO vehicle in the game uses; they are left out rather than guessed.
    int32_t steer_bam_per_unit() const {
        if (is_wheel()) return is_steered() ? 15 : 0;
        if (flags == 0x80) return -size;
        return 0;
    }
    bool is_right() const { return (flags & 0x01) != 0; }
    // `&FF`: a decorative piece carried by the body - tracks, the driver's head, a spare wheel.
    // It hangs where the line says and does nothing else: no steering, no rolling.
    bool is_decor() const { return flags == 0xFF; }
    // A powered moving part: the rotors (`&83`, counter-rotating on the UFO), the hovercraft's
    // fan and its rudders, the INFLATE's engine. 12 of them over 6 vehicles, every one resolving
    // to a shape of its own `.VHC`. They are drawn where their line says; `is_rotor()` says
    // which of them also turn.
    bool is_powered() const { return flags == 0x80 || flags == 0x81 || flags == 0x83; }
    // The ones that TURN: the helicopters' two rotors and the UFO's counter-rotating pair
    // (`&83`), the hovercraft's pusher prop (`&81`). 9 anchors over 4 vehicles.
    // ⚠️ `&80` is NOT one of them - it is the hovercraft's two rudders and the INFLATE's engine
    // block, and the authors' own comment on the rudders reads "&80 = colour follower". What it
    // does beyond taking the team colour is UNCONFIRMED, so nothing is invented for it.
    bool is_rotor() const { return flags == 0x81 || flags == 0x83; }
    // `&87`..`&8D`: the effect emitters - nitro, damage fire, the three smoke thresholds, the
    // explosion. Their shapes are never in the `.VHC`; they live in INGAME.WAP.
    bool is_effect() const { return flags >= 0x87 && flags <= 0x8D; }
    // The driver's head is `&40` with no index: its shape comes from the GLOBAL table, which is
    // the character the player picked, not from this `.VHC`.
    bool is_global_shape() const { return (shape_sel & 0x40) != 0; }
};

// One `ndial` line: a gauge the cabin view draws INTO the dashboard sheet every frame.
// `r[1]..r[10]` are the line's numbers in order (r[0] unused,
// so the indices match the spec); for every type but `copy`, r[1] is the SOURCE.
struct DashDial {
    enum Type { Copy = 0, Point = 1, Pill = 2, Block = 3, HArea = 4, VArea = 5 };
    int type = Copy;
    int32_t r[11] = {};
};
// The original keeps at most ten per player.
constexpr int kMaxDashDials = 10;

// One `vehicle <base> <count>` block. A `.VHC` has two: exterior then interior.
struct VehicleSection {
    std::string base;   // e.g. "JEEP" -> JEEP1..JEEP6 + JEEPX
    int count = 0;      // number of shapes under that name (7 = 6 colours + the wreck)
    int32_t internal[4] = {0, 0, 0, 0};
    std::vector<std::string> wheelsh;   // the `wheelsh` lines, in file order
    // What `shape_sel` actually indexes. NOT the `wheelsh` list: every name that owns the 7
    // team variants (`<stem>1..6` + `<stem>X`) is DEPLOYED into 7 consecutive slots, the same
    // way the body is. The authors wrote the arithmetic in their own comments - BUGGY reads
    // `wheelsh BUGLWHL1 ;4 = &84` then `wheelsh BUGRWHL1 ;4+7 = &8B` - so an index legitimately
    // runs past the number of `wheelsh` lines. Resolving against the raw list instead leaves
    // the BUGGY's three right wheels unresolved, and it drives on its left ones only.
    std::vector<std::string> shape_table;
    std::vector<WheelAttach> wheels;
    std::vector<DashDial> dials;   // the `ndial` lines - the interior section's, in practice
};

// One `hnd fx <category> <index>, [pre|keep] [env|lev|vhc|front] <sample> <6 numbers>` line.
//
// The developers left the whole trigger system in clear text in the vehicle scripts.
//
// The six numbers, confirmed against the shipped `.WAV` files:
//   loop_start, loop_end  in SAMPLES (8-bit mono, so also bytes of payload); -1 = end of file.
//                         Verified inside the sample on every engine and every looping effect -
//                         ATV_ENG is 23 095 samples and loops 8 340..12 248, TUR is 40 355 and
//                         loops 36 106..40 000, which is the tail whine of a turbo.
//   one_shot              1 = play once, 0 = loop. Every continuous sound in the game carries 0
//                         and every impact carries 1.
//   p4                    0 or -1 throughout. Meaning unknown; nothing here reads it.
//   freq                  playback rate in Hz. Usually 11025, the rate of the `.WAV`s, but not
//                         always: HOVCAR's engine is 13000 and HELI's landing 8000.
//   freq_full             ⚠️ HYPOTHÈSE: the rate at FULL INTENSITY, i.e. the pitch the engine
//                         reaches at maximum revs. Fits the data (ATV 12000->17000, BEETLE
//                         12000->35000); JEEP's 11025->11000 is a flat-pitch diesel drone, which
//                         is degenerate rather than contradictory. 0 = no variation.
struct FxSound {
    std::string name;          // sample stem as written, e.g. "skd_grav"
    std::string dir;           // "", "env", "lev", "vhc", "front" - which directory to look in
    bool preload = false;      // `pre` - loaded up front and kept
    bool keep = false;         // `keep` - loaded on first use, then cached
    int32_t loop_start = 0;
    int32_t loop_end = -1;
    bool one_shot = true;
    int32_t p4 = 0;
    int32_t freq = 11025;
    int32_t freq_full = 0;

    bool declared() const { return !name.empty(); }
};

// The seven `hnd fx` categories, in the order the original's own keyword list has them.
//
// ⚠️ The INDEX means different things per category, and that is the whole design:
//   Engine  always 0
//   Nitro   0 = the turbo loop, 1 = `notturb`, the "no boost left" refusal
//   Ground  the SURFACE TYPE 0-7 (the level's `land` table) - the rolling sound
//   Skid    the surface type as well - the tyres sliding
//   Land    the surface type - the thump on touchdown
//   Hit     the CONTACT TYPE. The scripts comment it themselves: `0 = solid, 1 = splat with
//           var = 0, 2 = same car, 3 = other car, 4/5/6 = splat objects with var 1/2/3`, and 7
//           is the explosion.
//   Check   0 checkpoint, 1 lap completed, 2 and 4 checkpoint missed, 3 wrong way, 5 a jingle
enum class FxCategory { Engine = 0, Nitro, Ground, Skid, Land, Hit, Check, Count };
constexpr int kFxCategoryCount = static_cast<int>(FxCategory::Count);
constexpr int kFxIndexCount = 8;

// The 8 ground-contact points: the corners of the body's .FT bounding box, in the ring order the
// engine builds them in. Corners 0-3 are the BOTTOM face - `hnd wheels 15` is
// exactly that mask.
//
// Ring order matters: the tipping masks below are only readable in it.
//   roll  = corners on xmin (0x C3) against corners on xmax (0x3C)
//   pitch = corners on zmax (0x66) against corners on zmin (0x99)
// ⚠️ THE SECTOR TABLE STARTS AT 0x16, NOT 0x14 - and getting that wrong is invisible until you
// drive at something head-on. The original reads each entry as a 32-bit load at
// 0x14 + sector*2 shifted right 16 bits, which is the SHORT at 0x16. The same idiom is all
// over the original (a 32-bit load at ft+2 shifted right 16 is the short at ft+4).
//
// Read from 0x14 instead, every sector is off by one AND sector 0 - the one an approach straight
// at an object uses - picks up the unrelated field before the table, which reads 0 on shape after
// shape. The result is an object with NO reach in the direction you are driving at it: the car
// goes through a tree, a signboard, a plank fence. The user's report of 2026-09-16, "the physics
// volume around it looks smaller than the object itself", was exactly this.
//
// The offsets also prove themselves: 0x16 + 32*2 = 0x56, where the normals start, and
// 0x56 + 32 = 0x76 = the file size. The whole record is accounted for, with nothing left over.
// Cross-check on the data: from 0x16, HAYBALE's sector 0 is 30 (its own z half-extent), TYRES' is
// 41 (its radius), TREE's is 160. From 0x14 all three read 0.
constexpr size_t kFtSectorReach = 0x16;
constexpr size_t kFtSectorNormal = 0x56;

struct VehicleBox {
    int32_t xmin = 0, xmax = 0, ybottom = 0, ytop = 0, zmin = 0, zmax = 0;
    std::array<std::array<int32_t, 3>, 8> corner{}; // game axes: +Y DOWN, so ybottom > ytop

    // The shape's own bounding radius, `.FT` +0x0c. The engine uses it for the SPATIAL QUERY
    // that builds a vehicle's candidate-object list every tick, not for contact.
    int32_t radius = 0;

    // THE 32-SECTOR COLLISION PROFILE, `.FT` 0x14..0x53 and 0x56..0x75 - the shape's silhouette
    // seen from above, one reach and one surface normal per 1/32 turn of its own frame. This is
    // the narrow phase of every vehicle-against-scenery contact, and the vehicle
    // brings its own half of it: the test compares the vehicle's reach in the direction of the
    // object against the object's reach in the direction of the vehicle.
    //
    // `sector_normal` is a BAM8 angle (byte x 256 = BAM16), the outward normal of the silhouette
    // over that sector. On a box shape it collapses to the four values 0/0x40/0x80/0xc0 - which
    // is how HAYBALE.FT reads, and that is what confirmed the table.
    //
    // ⚠️ These bytes OVERLAP the drivable-surface profile at 0x20-0x31. The `.FT` is a union: a
    // shape is either drivable scenery (ramp,
    // bridge, platform) or a blocking one, never both, and its category says which reading
    // applies. Do not try to make one struct hold both meanings at once.
    std::array<int32_t, 32> sector_reach{};
    std::array<uint8_t, 32> sector_normal{};

    void build();                       // fills `corner` from the six extents
    static VehicleBox from_ft(const Blob& ft); // reads the first six int16 of a .FT, plus +0x0c
};

constexpr uint16_t kCornerMaskRollA = 0xC3;  // xmin side
constexpr uint16_t kCornerMaskRollB = 0x3C;  // xmax side
constexpr uint16_t kCornerMaskPitchA = 0x66; // zmax side
constexpr uint16_t kCornerMaskPitchB = 0x99; // zmin side

struct VehicleTuning {
    std::string name;

    SkidPair fvel[kLandTypeCount], fdam[kLandTypeCount], brk[kLandTypeCount];
    SkidPair svel[kLandTypeCount], yavel[kLandTypeCount], iavel[kLandTypeCount];
    int32_t bounce[kLandTypeCount][4] = {};

    TilterAxis tilter_z, tilter_x;

    // In FILE order. Use the accessors: the parser writes them to slots 0,1,5,2,3,4.
    int32_t cogs[6] = {};
    int32_t nitro[5] = {};
    // Every `hnd fx` line this vehicle declares, by category and index. An entry whose name is
    // empty was not declared - the original simply has nothing to play for that event.
    FxSound fx[kFxCategoryCount][kFxIndexCount];
    int32_t steering[6] = {};
    int32_t acceling[2] = {};
    int32_t rpush[4] = {};
    int16_t wheels[3] = {};
    int32_t damage = 0;

    std::vector<GearRow> gear_f, gear_b;
    std::vector<VehicleSection> sections;
    // `dash <TM>`: the dashboard sheet the interior's faces show and the `ndial`s draw into.
    std::string dash;
    VehicleBox box;
    // Radius of the vehicle's wheel mesh, from that shape's own .FT bounding box. Purely for
    // rolling the wheels at the right rate; 0 when the vehicle has none.
    int32_t wheel_radius = 0;

    // --- cogs, by the slot the engine reads, not by position in the line ---
    int32_t roll_righting() const { return cogs[0]; }   // slot 0 -> the roll rate
    int32_t pitch_righting() const { return cogs[1]; }  // slot 1 -> the pitch rate
    int32_t contact_tolerance() const { return cogs[2]; } // slot 5
    int32_t gravity() const { return cogs[3]; }         // slot 2, added to vy per tick
    int32_t air_pitch_rate() const { return cogs[4]; }  // slot 3
    int32_t air_pitch_limit() const { return cogs[5]; } // slot 4

    // --- the three categories, read from the data rather than from the name ---
    bool needs_drive_contact() const { return wheels[0] != 0; }
    bool needs_grip_contact() const { return wheels[1] != 0; }
    bool is_aircraft() const { return nitro[0] == 3; }

    const SkidPair& skid(const SkidPair (&t)[kLandTypeCount], int land) const {
        return t[land & (kLandTypeCount - 1)];
    }
};

// Builds every section's `shape_table` and resolves every `wheel` line's `shape` against it.
// `has_variants` answers whether a shape name owns the 7 team variants; `load_vehicle` answers
// it from the `.VHC` itself, which is the truth. Called by the parser with a name-only guess so
// a script parsed without its archive still resolves.
void resolve_wheel_shapes(VehicleTuning& out,
                          const std::function<bool(const std::string&)>& has_variants);

// Parses one `<NAME>.TXT` out of an already-unpacked `.VHC`, applying the broadcast rule.
// `box` is left zeroed if the matching `<BASE>1.FT` is absent.
bool parse_vehicle_script(const std::string& txt, const std::string& name, VehicleTuning& out);

// Loads `<game_dir>/<NAME>.VHC`, parses its script and its body `.FT`. Returns false on any
// failure, leaving `out` untouched. `archive_out` receives the shared archive, for the model.
bool load_vehicle(const std::string& game_dir, const std::string& name, VehicleTuning& out,
                  ArchivePtr* archive_out = nullptr);

} // namespace bigred
