// `<LEVEL>.TXT` script directives that drive the world at run time.
//
// Only the decor animation (`anim`) lives here; `texes` is parsed next to the terrain code that
// consumes it (see terrain.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bigred {

// The game's timer tick, in milliseconds - CONFIRMED in the game's own TIMER.DLL: its
// `TIMER_Init_` export starts a periodic 14 ms multimedia timer whose handler
// does nothing but increment the exported `_TIMER_Count`. The original reads that counter once a
// frame and works in its deltas, so one tick is exactly 14 ms (~71.4 Hz). Every `anim` period
// below is counted in these ticks.
constexpr int kGameTickMs = 14;

// ⚠️ The VEHICLE PHYSICS does NOT run on every timer tick: it runs on every OTHER one.
//
// CONFIRMED in the original. Its main loop catches up one iteration per
// elapsed timer tick, and each iteration increments a counter before calling the
// per-vehicle update. That update guards the physics with a test of the counter's
// low bit - so the physics step only runs when the
// counter is EVEN. Other subsystems are gated the same way at other rates (`& 7` = every 8).
//
// So one physics step is 28 ms (~35.7 Hz), and EVERY per-tick quantity of the vehicle
// physics - every `hnd skid` rate, the gravity, the boost duration - is
// per 28 ms. Stepping them at 14 ms makes the whole game run at exactly double speed, which is
// how this was found: the user reported "it feels twice too fast" while jumps still landed
// correctly - and a ballistic arc's SHAPE is independent of the tick rate, so that pair of
// symptoms points at the rate and nothing else.
constexpr int kPhysicsTickMs = kGameTickMs * 2;

// One `anim <period>, <count>, <mask>, <v0>,..,<v(count-1)>` line of a level script.
//
// The original keeps these in a 49-byte record per line (a tick
// accumulator, the period, the count, the cursor, the mask and
// the values). Once a frame, it does
// `acc = (acc + elapsed_ticks) % (count * period)` then `cursor = acc / period`, and per
// instance it applies `variant = (variant & ~mask) | values[cursor]`.
struct LevelAnim {
    int period = 1;               // how many ticks each value is held
    uint8_t mask = 0;             // which bits of an instance's variant byte this line owns
    std::vector<uint8_t> values;  // the cycle, one value per step

    int step_count() const { return static_cast<int>(values.size()); }
    int cycle_ticks() const { return period * step_count(); }
    int step_ms() const { return period * kGameTickMs; }   // wall-clock duration of one step
    int cycle_ms() const { return cycle_ticks() * kGameTickMs; }

    // Which step is showing `elapsed_ms` after the level started. The original's accumulator is
    // shared by every instance on the slot, so all of them are always in phase.
    //
    // FRAME-RATE INDEPENDENT BY CONSTRUCTION, and so is the original: its accumulator counts
    // TIMER ticks (a 14 ms multimedia timer), never rendered frames, so the animation runs at
    // the same wall-clock speed whatever the frame rate. We go one step further and read the
    // clock directly instead of accumulating per frame - `(T % (p*n)) / p == (T / p) % n` for
    // integers, so this is the same sequence with no accumulator to drift and nothing that
    // changes between 10 fps and 240 fps.
    //
    // NOT ported: the original clamps a frame's elapsed ticks to 35, so on a machine too slow to
    // keep up its animations quietly ran in slow motion. That is a 1996 artifact of catching up
    // inside the frame loop, not an intent.
    int step_at(uint32_t elapsed_ms) const;
};

// Parses every `anim` line of a level script, in file order. That order IS the slot numbering:
// an instance picks its line with MAP.DAT offset 21 modulo 10 (see anim_slot_of).
std::vector<LevelAnim> parse_anim_directives(const std::string& level_txt);

// The level's `land` line: 16 entries mapping a MAP.MPC high nibble (already remapped through
// MAP.CHT) to a surface type 0-7 - TRACK GRIPPY SLIPPY CLIFFS GRASS SAND RAMPS AIR. The same
// index selects the vehicle's `hnd skid` sub-table, which is the whole link between the ground
// and the handling. The line spills over as many source lines
// as it likes; missing entries stay 0 (= TRACK), which is what an unwritten slot means.
std::vector<uint8_t> parse_land_table(const std::string& level_txt);

// The level's `grid` line: the starting grid, and it is NOT a list of eight places.
//
// CONFIRMED at both ends, in the original. The `grid` directive
// reads THREE numbers into three globals - the grid's own origin x, z and heading - and then
// exactly TWENTY-FOUR more into a word array: eight triplets. The consumer,
// the original's grid placement, picks which eight with `two_players ? slot*3 : slot*3 + 6`:
//
//   the FIRST TWO triplets are the two-player layout,
//   the LAST SIX are the layout for everyone else - so SIX racers, not eight.
//
// That matches the rest of the data: the race loop runs over seven vehicle records, the seventh
// being the presentation helicopter the `car` list also names (FEHELI), and every `.LEV` ships
// `0.REC`..`5.REC` plus a smaller `6.REC` for that helicopter's fly-by.
//
// A triplet is (a, b, dyaw), all in the grid's own frame; see grid_slot() in sim/race.h for the
// placement, which is a rotation by the grid heading and nothing else.
//
// ⚠️ An older reading of this line had it as "up to eight slots of (x, z, cap)" and took the
// first triplet as the only start point. The first triplet is an OFFSET, not a position; the
// position is the three numbers before it.
struct StartingGrid {
    bool present = false;
    int32_t x = 0, z = 0;      // the grid's origin, world units
    int32_t yaw_bam16 = 0;     // the heading every slot inherits
    struct Slot {
        int32_t a = 0, b = 0;  // the slot's offset in the grid's frame
        int32_t dyaw = 0;      // added to the grid heading, BAM16
    };
    Slot two[2]{};             // the two-player layout
    Slot six[6]{};             // three racers and up - the retail grid
    // OURS, not the data's: places 6..11 of a network race, chosen and checked against the
    // terrain by extend_grid() (sim/race.h). Past `extra_count`, grid_slot() copies the six
    // right behind themselves, unchecked.
    Slot extra[6]{};
    int extra_count = 0;
};
StartingGrid parse_grid(const std::string& level_txt);

// `heli <x>,<z>,<cap>` - where the presentation helicopter (racer 6, FEHELI or UFO) starts the
// track's fly-by: the original puts it there, on the ground, instead of on a grid place, when the
// fly-by flag is set and the x is not 0. Cap in BAM16, as `grid`.
struct HeliPoint {
    bool present = false;
    int32_t x = 0, z = 0, yaw_bam16 = 0;
};
HeliPoint parse_heli(const std::string& level_txt);

// `horizobj <SHAPE> <yaw>,<pitch>,<dist>, <ax>,<ay>,<az>, <flags>` - an object of the far
// background, kept at a fixed offset from the CAMERA so it never comes nearer: DIRT1's SUNRISE,
// MOON3's BACKDROP (the Earth). Up to 20 of them. The offset is `dist` along yaw/pitch (BAM16).
// A negative `flags` gives the variant byte -flags.
struct HorizonObjectDef {
    std::string shape;
    int32_t yaw = 0, pitch = 0, dist = 0;
    int32_t ax = 0, ay = 0, az = 0;
    int32_t flags = -1;
};
std::vector<HorizonObjectDef> parse_horizon_objects(const std::string& level_txt);

// The level's `fx <a>, <roll>, <general>` line: how many of each spoken sample the circuit's
// folder holds, `<NAME>1.WAV` to `<NAME><n>.WAV`. `a` is packed, its
// low byte the JUMP count, the next the DEAD count, the third the FINISH count (CHOPPER1's
// `&000700` is its seven DEAD files, RIG3's `&010003` its one FINISH). 0 means "say nothing",
// even when files are there: CHOPPER2 ships three JUMP files and writes 0.
struct LevelCommentary {
    int jump = 0, dead = 0, finish = 0, roll = 0, general = 0;
};
LevelCommentary parse_commentary(const std::string& level_txt);

// The level's `laps` line - how many times round to win. Absent on most circuits, which means
// the value the menu chose; `kDefaultLaps` is what we use until there is a menu.
constexpr int kDefaultLaps = 3;
int parse_laps(const std::string& level_txt);

// The level's `light <elevation>,<azimuth>` line: where the SUN is, as two BAM16 angles - the
// pitch and the yaw of the light object the original plants. Nearly every circuit
// says `&1000,-&2000` (22.5 degrees up); CITY1, CITY4, RIG3/RIG4 turn it. Three levels have no
// line (CHOPPER3, DIRT3, TEST1) and take the common one.
struct SunLight {
    int32_t elevation = 0x1000;
    int32_t azimuth = -0x2000;
};
SunLight parse_light(const std::string& level_txt);

// The level's `timing <t>,<n>` line, converted to 14 ms TIMER TICKS, which is what the original
// counts it in: `countdown = (t * 70) / 11025 + max(n, 1)`, exactly as the original computes it.
//
// ⚠️ This is the START COUNTDOWN, and that is not a guess: the original runs the vehicle physics
// only while the countdown is negative, and the countdown starts at this value and is decremented
// once per timer tick. Nothing moves until it goes negative. On the way down it fires
// a beep at 210, 140, 70 and 0 ticks left - 2.94 s, 1.96 s, 0.98 s and go.
//
// Across the shipped circuits it lands between 204 ticks (SNOW3, 2.9 s) and 763 (RIG4, 10.7 s).
// Two levels have no `timing` line at all (CITY3, TEST1) and get `kDefaultCountdownTicks`.
constexpr int kDefaultCountdownTicks = 300;
int parse_countdown_ticks(const std::string& level_txt);

// The level's `unsafe <n>` lines: gate `n` is not a place to be put back on the track. The
// directive clears the flag byte at the end of checkpoint record `n`, and the only reader is
// the respawn's backward search, which
// steps past any gate whose flag is clear. See sim/race.h.
std::vector<int> parse_unsafe_gates(const std::string& level_txt);

// The level's `car <NAME>` lines, in file order: which vehicle types this circuit allows.
std::vector<std::string> parse_car_list(const std::string& level_txt);

// True for the vehicle a `car` list names only for the track FLY-BY, never to be raced:
// `FEHELI`, the presentation helicopter of the Earth circuits, and `UFO`, its counterpart on
// the MOON circuits (the user, who knows the game, 2026-09-26). Every list a player picks from
// drops them.
bool is_presentation_vehicle(const std::string& name);

// `fixcam x, y, z ;NAME` - the circuit's TRACKSIDE CAMERAS, one line each, 17 to 28 per circuit,
// named by the authors in the comment ("STARTING GRID", "HILL JUMP #2", "ROOFTOP CAM #1").
// The original reads the three numbers into a table of triplets and
// counts them. World units, game frame: x and z like the `grid` line, y with +Y
// DOWN (every shipped camera is negative: above the ground).
struct FixedCamera {
    int32_t x = 0, y = 0, z = 0;
    std::string name;   // the comment, trimmed; may be empty
};
std::vector<FixedCamera> parse_fixed_cameras(const std::string& level_txt);

// The level's `nitros <count>,<ticks>` line - how many boosts a car starts the race with, and
// what a full nitro gauge is worth in physics ticks. Defaults to {0, 0} when the line is absent.
//
// The second value is ALWAYS exactly 200 x the first across all 26 levels, and 200 is `nitro[1]`,
// the boost duration in ticks of every ground vehicle: it is the gauge's full scale, i.e. the
// total boost TIME the reserve represents, not a second independent quantity. Only the count is
// the simulation's input (it initialises the vehicle's reserve, decremented once per
// boost); the ticks figure is for drawing the bar. Range across the game: 2 (the SNOW
// circuits, CHOPPER2) to 10 (TEST1); 7 on every CITY circuit.
struct NitroReserve {
    int count = 0;
    int full_scale_ticks = 0;
};
NitroReserve parse_nitros(const std::string& level_txt);

// One `spot <type> <SHAPE> <speed>,<frames>,<land>` line of an ENVIRONMENT script.
//
// ⚠️ These live in the `.ENV`, not in the level's own `.TXT`: one declaration per family of
// circuits (DIRT.ENV, CITY.ENV, MOON.ENV...), which is why every DIRT track kicks up the same
// dust and only the WATER tracks splash. `DIRT.ENV` carries the authors' own key for the
// grammar.
//
// The original keeps these in a 5-dword record: type, shape index, speed, frame count, land
// type - in that order.
struct SpotEffect {
    int type = 0;           // 1 = skidding, 2 = driving, 3 = impact spark
    std::string shape;      // DUST1_FX, SNOW1_FX, RUBBERFX, BIGSPL, SPLASH2, SPARK_FX
    int speed = 1;          // GAME TICKS (14 ms) one flipbook frame is held
    int frame_count = 0;    // the last frame index; the particle dies once it passes this
    int land = -1;          // index into the level's `land` table, -1 = every surface
};

// Parses every `spot` line of an environment script, in file order.
std::vector<SpotEffect> parse_spot_effects(const std::string& env_txt);

// The `.ENV` a level draws its effects (and its shared decor) from: the level name with its
// trailing digits removed - DIRT1 -> DIRT, CHOPPER3 -> CHOPPER. Confirmed against the shipped
// set: every level name in the game is a family name plus one digit, and there is exactly one
// `.ENV` per family.
std::string env_name_for_level(const std::string& level_name);

// Which `anim` slot an instance follows, or -1 if its collision category is not one the engine
// animates.
//
// The original's per-frame decor updater dispatches on the instance's category through a
// 37-entry table; the cases that reach the animation applier are table indices
// 6, 7, 8 (raw categories 10, 20, 30), 0x1d (raw 252), and 9 (raw 40) - the last one only when
// the variant byte's bit 0x40 is CLEAR, since a set bit routes it to a separate per-instance
// counter instead. No shipped level has that bit set on an animated category, so that counter is
// dead code in the retail data; the gate is kept because it is what the original tests.
int anim_slot_of(uint8_t raw_category, uint8_t variant_byte, uint8_t map_dat_offset21);

} // namespace bigred
