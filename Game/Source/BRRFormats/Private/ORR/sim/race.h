// The race itself: the starting grid, the countdown, the checkpoint ring, laps and placing.
//
// Everything here is part of the SIMULATION, not of the display: it is integer-only, it advances
// on the fixed tick, and it is identical on every machine, because lockstep networking needs the
// whole race - not just the cars - to agree.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats/level_script.h"
#include "sim/vehicle.h"

namespace bigred {

// How many of the original's 14 ms ticks of going backwards raise the WRONG WAY warning.
// The original fires its event when its counter reaches 0x8c - two seconds.
constexpr int kWrongWayTicks = 140;

// One checkpoint gate: a SLAB OF ROAD. Seen from above it is a trapezoid lying across the
// track - `length` wide from one verge to the other, `2 * half_depth` deep in the direction the
// car drives, and the depth tapers from one side of the road to the other, which is how a gate
// on a bend is deeper on the outside of the curve.
//
// ⚠️ IT IS NOT A CORRIDOR ALONG THE ROAD, and reading it as one is a quarter-turn error that
// takes the whole ring off the track. See the field notes below.
//
// ⚠️ THIS IS WHAT `MAP.CSE` IS. It was catalogued for a long time as "cloud placement, partially
// resolved"; it is the checkpoint table, and the proof is a single call site. The original's
// `map` directive builds the level's file names by appending one suffix after another - `.DAT`,
// `.MPH`, `.MPC`, `.CHT`, `.GRP`, then `.CSE`, then `.COL` - and the loader it hands the `.CSE`
// name to fills a table with 16 bytes per record and a flag byte
// after them. That table is the one the gate test, the lap counter and the respawn all read.
struct Gate {
    // The anchor: ONE SIDE of the road (the across-track origin), half way through the slab.
    int32_t x = 0, z = 0;
    int16_t angle = 0;         // heading, 1024 units per turn - a raw trig-table index
    // ⚠️ ACROSS the track, not along it: the span from the anchor's verge to the other one.
    // 3 700 to 12 000 units on the shipped levels, against a road 2 000 units wide - a gate
    // deliberately reaches well past the verges, because its job is to locate a car that has
    // left the road as much as one that has not.
    int16_t length = 0;
    int16_t half_depth = 0;        // half the slab's depth ALONG the track, at the anchor's side
    int16_t half_depth_delta = 0;  // added to it linearly at the far side, so it is a trapezoid
    bool safe = true;          // cleared by the level's `unsafe <n>` lines: not a respawn place
    // The gate a `CHECKP` line is attributed to (see CheckLine). The ring is fine-grained - 14
    // to 69 gates a lap - because its job is to know where a car is, for the placing and for
    // putting it back on the track; a checkpoint is FIRED by touching the line itself, which
    // can be thousands of units from this gate's entry face. This mark only tells the respawn
    // and the overlay where the line is. Set by mark_checkpoint_gates().
    bool checkpoint = false;
};

// Reads the whole ring out of a level's `MAP.CSE`. The stride is 16 and every shipped level
// divides exactly: 14 gates on DIRT2 up to 69 on WATER2.
std::vector<Gate> parse_gates(const uint8_t* map_cse, size_t bytes);

// Applies the level's `unsafe` lines to a ring `parse_gates` returned.
void mark_unsafe_gates(std::vector<Gate>& gates, const std::vector<int>& unsafe_indices);

// A CHECKPOINT LINE - one `CHECKP` instance of `MAP.DAT`, and the thing that actually fires a
// checkpoint. Not a gate.
//
// ⚠️ THE GATES DO NOT TRIGGER CHECKPOINTS. The original's collision router sends
// categories 10 and 14 (raw bytes 50 and 54, used by `CHECKP` alone) to its checkpoint routine,
// with the instance that was touched. That function is the whole checkpoint and lap logic, and it reads
// the INSTANCE, never the ring:
//   * `.FT` makes it an invisible oriented box, 2048 across and 100 thick - a line over the road;
//   * the variant byte (offset 19) is its ORDINAL in the lap, 0 being the
//     start/finish line, bit 0x80 on the lap's last one;
//   * offset 21 goes to the vehicle's end of the gate window.
// Several instances can share an ordinal: they are pieces of the same line, laid side by side
// to cover a wide road. Read by the bench's RACE block on all 25 circuits: the ordinals are
// contiguous from 0 and the flagged one is always the highest.
struct CheckLine {
    int32_t x = 0, y = 0, z = 0;
    int32_t yaw_bam16 = 0;
    int32_t box_xmin = -1024, box_xmax = 1024, box_zmin = -50, box_zmax = 50;  // `.FT` 0x18..0x1e
    int32_t y_top = -500;      // `.FT` 0x06, +Y down: the top of the box, relative to `y`
    uint8_t number = 0;        // the raw ordinal byte, bit 0x80 included
    uint8_t window = 0;        // offset 21 - the gate the progression window reaches to
    // Category 54 (index 14) passes the vertical test first - the one every obstacle has -
    // and category 50 (index 10) does not. The helicopter circuits use 54: a line you fly OVER
    // does not count.
    bool height_gated = false;
    int gate = -1;             // the gate it is attributed to - respawn and overlay only
    bool in_gate = false;      // false when no gate contains it and `gate` is the nearest one
    int ordinal() const { return number & 0x7f; }
    bool last() const { return (number & 0x80) != 0; }
};

// Every `CHECKP` instance of the level, as a CheckLine, in instance order - which is the order
// the race tests them in, so that it is the same on every machine. `ft_lookup` resolves a shape
// name to its `.FT` bytes, as for build_sim_objects.
std::vector<CheckLine> build_check_lines(
    const std::vector<SceneryInstance>& instances, const std::vector<std::string>& shape_names,
    const std::function<const Blob*(const std::string&)>& ft_lookup);

// The gate a point on the track belongs to: the containing gate whose ENTRY FACE is closest
// behind it (gates overlap, so there are usually several), or failing that the gate whose
// middle of the road is nearest. `inside` says which of the two answered.
int gate_of_point(const std::vector<Gate>& gates, int32_t x, int32_t z, bool* inside = nullptr);

// Attributes each line to a gate (CheckLine::gate) and marks those gates `checkpoint`, gate 0
// included. The marks are for the overlay and the respawn; nothing FIRES from them any more.
void mark_checkpoint_gates(std::vector<Gate>& gates, std::vector<CheckLine>& lines);

// Whether a car's body touches a line this tick - the original's box against the body box,
// behind the vertical test for a height-gated line.
bool check_line_touched(const CheckLine& line, const VehicleSim& car);

// A gate's heading as the VEHICLE states it. The two conventions differ by a quarter turn, and
// this is not inferred: the original's own respawn writes `yaw = (angle << 6) + 0x4000` into the
// vehicle record, which is exactly this.
inline int32_t gate_yaw_bam16(const Gate& g) {
    return ((static_cast<int32_t>(g.angle) << 6) + 0x4000) & 0xFFFF;
}

// Where a point falls in a gate - the port of the original's gate test.
//
// ⚠️ The two axes are named for what they MEASURE, because the original's own names would be
// the wrong way round: the coordinate bounded by `length` runs ACROSS the road.
struct GateProbe {
    bool inside = false;
    int32_t across_track = 0;  // 0 at the anchor's verge, `length` at the far one
    int32_t along_track = 0;   // signed, 0 half way through the slab, + in the driving direction
    int32_t half_depth = 0;    // the slab's half-depth at that across-track position
    int32_t lateral = 0x2000;  // `along_track` mapped to 0..0x4000, 0x2000 being half way
};
GateProbe probe_gate(const Gate& g, int32_t x, int32_t z);

// The world (x, z) of the point `across_track` units from the anchor's verge and `along_track`
// units through the slab. This is the original respawn's own placement formula with its two arguments
// left open, and it is the exact inverse of probe_gate: everything that needs a point OF a gate
// rather than a test AGAINST one goes through it, so the trig convention lives in one place.
// The gate overlay draws the ring with it.
void gate_point(const Gate& g, int32_t across_track, int32_t along_track, int32_t& x, int32_t& z);

// The slab's half-depth at an across-track position - it is a trapezoid, so this is not
// `half_depth` except at the anchor's own verge.
int32_t gate_half_depth_at(const Gate& g, int32_t across_track);

// Where a vehicle goes when it is put back on the track at `gate` - the original's own point:
// **the middle of the road** (`length / 2` across the track), at the depth through the gate it
// last held. `lateral` is the 0..0x4000 figure GateProbe carries.
RespawnPoint gate_respawn(const Gate& g, int32_t lateral);

// One racer's progress round the ring.
struct RacerProgress {
    int gate = -1;               // the gate it is standing in, -1 when it is off the ring
    int32_t lateral = 0x2000;    // how far through that gate, in the driving direction
    int reached = -1;            // the furthest gate REACHED this lap - what placing reads
    int32_t reached_lateral = 0x2000;
    int32_t along_permille = 0;  // how far into `reached` the car is, 0..1000 - the tie-break
    int lap = 0;                 // laps completed
    bool finished = false;

    // --- what makes a lap COUNT -------------------------------------------------------------
    //
    // ⚠️ CROSSING THE LINE IS NOT ENOUGH: every gate carrying a `CHECKP` gantry has to have been
    // passed since the last crossing. Reported by the user as the original's own rule - "il ne
    // fallait en rater aucun, sinon ça ne validait pas, et si on en avait raté un, le jeu nous
    // le disait et ALT nous ramenait juste devant" - and it is what the `MISSED CHECK` banner in
    // GAMESVGA.SPR is for.
    //
    // It is also what stops the FIRST crossing counting a lap. The grid stands behind gate 0, so
    // a car starts the race already inside the LAST gate of the ring; moving from there into
    // gate 0 wraps the index and looks exactly like completing a lap. It is not one - no
    // checkpoint has been passed yet.
    //
    // On a level with `CHECKP` lines this is the original's own state, byte for byte:
    uint8_t last_check = 0x80;   // the last line taken IN ORDER. 0x80 = none yet
    uint8_t last_touched = 0xff; // the last line touched at all, in order or not
    // The GATE WINDOW, [window_start, window]: once a line has been taken, progress
    // is recorded only in a gate between these two, read round the ring. Each line taken in
    // order slides it on: the old end becomes the start, the line's offset-21 byte the end
    // (about the gate of the NEXT line). See RaceState::update_progress.
    uint8_t window_start = 0;
    uint8_t window = 0;          // the window's end
    // Set by the first line of the race: the next gate found inside the window is progress,
    // whatever its number - the original's best-lap marker at -1. See touch_line.
    bool take_next_gate = false;
    // Set when a line AHEAD of the expected one has been touched - which proves the expected
    // one was driven past - and cleared by the next line taken in order. Ours, not the
    // original's: it stands in for the gate window when ALT has to find the skipped line.
    bool skipped_line = false;
    // ... and on a level WITHOUT any (CITY3, the RUF circuits) the ring decides alone, with
    // this set standing in for it - see arm_checks().
    std::vector<uint8_t> checks_seen;  // parallel to the ring, 1 = passed since the last line
    // How many checkpoints are still to be taken before the line counts - for the debug trace.
    int checks_missing = 0;
    bool line_crossed_once = false;    // the first crossing STARTS lap 1, it does not end one
    uint32_t lap_start_tick = 0;       // when the current lap began, for the lap time
    // Set on the tick the line is crossed with checkpoints still unseen, and left set: the HUD
    // shows the banner for a while after, the way the original holds it for 0x15e game ticks.
    bool missed_check = false;
    uint32_t missed_check_tick = 0;

    // --- driving the wrong way - the port of the original's wrong-way counter ----------------
    //
    // Called on every exit of the original's progression function, it compares this tick's
    // (gate, lateral) with last tick's and counts the GAME TICKS spent going backwards round the
    // ring. At 140 of them - two seconds - it fires the event that plays `hnd fx check 3`. Going
    // forwards resets the count to zero, so a single tick of noise cannot trigger it; and the
    // one tick where the index legitimately wraps past gate 0 cannot either.
    int wrong_way_ticks = 0;
    // The previous tick's position on the ring, which is all the comparison needs.
    int prev_gate = -1;
    int32_t prev_lateral = 0x2000;
    uint32_t finish_tick = 0;               // when, in 14 ms ticks from the green light
    std::vector<uint32_t> lap_ticks;        // one entry per completed lap, same unit
    int place = 1;                          // 1 = leading

    // True on the tick the line was crossed WITHOUT every checkpoint - see `missed_check`.
    bool missed_check_now = false;
    // True on the tick the next line is driven PAST without being touched - the original's
    // event for `hnd fx check 4`. See RaceState::tick.
    bool missed_window_now = false;
    // True on the ONE tick the wrong-way count reaches its threshold, so the sound fires once.
    bool wrong_way_now = false;
    // True while the banner should be up: the count is at or past the threshold.
    bool wrong_way = false;

    // Events, each true for ONE tick: the racer crossed into a new gate; it touched the next
    // `CHECKP` line in order; it completed a lap. The SOUND is
    // `checkpoint_crossed`, never `gate_crossed`: the ring is far too fine to beep at.
    //
    // `missed_check_now` above is the original's other event: a line touched OUT of order - one skipped - and
    // not the same one as last touched, so brushing a line twice is silent.
    bool gate_crossed = false;
    bool checkpoint_crossed = false;
    bool lap_crossed = false;
};

// The whole race. One instance per session; the vehicles stay where they are.
class RaceState {
public:
    // `racers` is how many cars are on the grid, which is also what picks the grid layout.
    // `lines` are the level's `CHECKP` instances (build_check_lines); with none, the ring alone
    // decides the laps.
    void configure(std::vector<Gate> gates, int laps, int countdown_ticks, int racers,
                   std::vector<CheckLine> lines = {});

    // Puts the countdown back to the start line and forgets every lap. Does not touch vehicles.
    void restart();

    // --- what the rest of the game asks -------------------------------------------------------
    bool started() const { return countdown_ < 0; }    // the original's own test
    int countdown_ticks_left() const { return countdown_ > 0 ? countdown_ : 0; }
    // The countdown as the screen says it: 3, 2, 1, then 0 for GO. -1 once the race is running.
    int countdown_number() const;
    // True on the one tick the count changes, so a beep fires exactly once. The original beeps
    // at 210, 140, 70 and 0 ticks left.
    bool countdown_beeped() const { return beeped_; }
    bool race_over() const { return over_; }

    int laps() const { return laps_; }
    const std::vector<Gate>& gates() const { return gates_; }
    const std::vector<CheckLine>& check_lines() const { return lines_; }
    // Ticks since the green light, in 14 ms units. Zero while the countdown runs.
    uint32_t race_ticks() const { return race_ticks_; }
    const RacerProgress& progress(int racer) const { return progress_[static_cast<size_t>(racer)]; }
    int racer_count() const { return static_cast<int>(progress_.size()); }

    // How far round the lap a racer is, 0..1 in thousandths - the quantity the placing sort and
    // a progress bar both want, and it is integer.
    int32_t progress_permille(int racer) const;

    // --- the tick -----------------------------------------------------------------------------
    // Advance the race by ONE PHYSICS TICK (28 ms), which is two of the original's 14 ms timer
    // ticks - the countdown and the lap clock are counted in those, like every other duration in
    // the game. `cars` holds one pointer per racer, in grid order; a null entry is a slot nobody
    // is driving.
    void tick(const VehicleSim* const* cars, int count);

    // Where racer `slot` starts. `grid` is the level's own line; the layout it picks depends on
    // how many racers `configure` was told about.
    RespawnPoint start_of(const StartingGrid& grid, int slot) const;

    // Where a racer goes when it asks to be put back on the track: the middle of the last gate
    // it reached, at the lateral position it held there. The original walks BACKWARDS from that
    // gate over any the level marked `unsafe`, so a checkpoint sitting on a jump or a bridge
    // never becomes a respawn point; that walk is reproduced, minus its "is the spot occupied"
    // retry, which needs the other cars.
    //
    // ⚠️ WITH ONE EXCEPTION, and it is the user's own description of the game: if the racer has
    // gone PAST a checkpoint without registering it, that checkpoint is where it goes back to -
    // "si on en avait raté un, le jeu nous le disait et ALT nous ramenait juste devant".
    RespawnPoint respawn_for(int racer, const RespawnPoint& fallback) const;

    // The same, with the original's "is the spot occupied" retry: while `occupied` says the
    // spot is taken, the car is moved 0x100 of the gate's lateral scale BACK
    // through it, toward its entry face; past the entry face it goes on from the EXIT face of
    // the previous safe gate. `gate` is where it ended up, -1 for the fallback - hand it to
    // on_respawned. A null `occupied` is the plain respawn_for above.
    struct RespawnChoice {
        RespawnPoint at;
        int gate = -1;
    };
    using SpotTaken = bool (*)(const RespawnPoint& at, const void* ctx);
    RespawnChoice respawn_choice(int racer, const RespawnPoint& fallback, SpotTaken occupied,
                                 const void* ctx) const;

    // Which gate respawn_for would use, or -1 if it would fall back. Public because the game
    // needs it to tell the race the move actually happened (see on_respawned).
    int respawn_gate(int racer) const;

    // Call this AFTER a racer has actually been put back on the track, on the same tick.
    //
    // ⚠️ IT IS NOT COSMETIC. Moving a car back to a gate it had already gone past, without
    // moving its progress marker with it, leaves the ring reading every metre of the return
    // trip as driving the wrong way - the car can never register that stretch again, and on a
    // missed checkpoint it could never be recovered at all. This snaps the marker to where the
    // car now is.
    void on_respawned(int racer);
    // ... at the gate respawn_choice actually picked, which the retry may have moved back.
    void on_respawned(int racer, int gate);

private:
    friend struct SnapshotAccess;   // sim/snapshot.cpp
    void update_progress(int racer, const VehicleSim& car);
    // What touching one `CHECKP` line does to a racer, as in the original.
    void touch_line(RacerProgress& p, const CheckLine& line);
    // A lap completed - shared by both paths, lines and ring.
    void complete_lap(RacerProgress& p);
    // Re-arms a racer's checkpoint set for a fresh lap - see RacerProgress::checks_seen.
    void arm_checks(RacerProgress& p) const;
    // The gate of the next line a racer has to take, or -1 - see respawn_gate.
    int expected_line_gate(const RacerProgress& p) const;
    // The gate "just before" the line of that ordinal: the one containing it, or one gate
    // before the nearest when no gate does. -1 if the level has no such line.
    int line_gate(int ordinal) const;

    std::vector<Gate> gates_;
    std::vector<CheckLine> lines_;
    int last_ordinal_ = -1;   // the highest ordinal among lines_, -1 without any
    std::vector<RacerProgress> progress_;
    int laps_ = kDefaultLaps;
    int countdown_start_ = kDefaultCountdownTicks;
    int countdown_ = kDefaultCountdownTicks;
    uint32_t race_ticks_ = 0;
    bool beeped_ = false;
    bool over_ = false;
};

// The grid placement on its own, so a tool can ask for it without a race - the port of
// the original's grid placement. `racers` picks the layout: exactly two takes the two-player triplets, anything
// else takes the six.
RespawnPoint grid_slot(const StartingGrid& grid, int racers, int slot);

// Places 6..11 for a network race of more than six, which the data does not have: the level's
// six places copied as a block - behind themselves, then beside, then further behind - keeping
// only the places whose ground is level and flat, with no obstacle within 350 units (a ramp 400) and no other
// place within 400. Integer only.
// `probe`, when given, is a vehicle put on every candidate and left 60 ticks with no input: a
// place it drifts off (more than on place 0, plus 40 units) is refused. It is the check that
// counts - a flat, clear place can still slide a car off (MOON2).
void extend_grid(StartingGrid& grid, const SimTerrain& terrain,
                 const VehicleTuning* probe = nullptr);
// How many racers the level can start: six, plus the places extend_grid() found good.
int max_grid_racers(const StartingGrid& grid);

} // namespace bigred
