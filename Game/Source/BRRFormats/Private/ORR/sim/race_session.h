// A whole race, simulated: every racer's vehicle, the race state, and the shared destruction
// state, advanced one 28 ms tick at a time from ONE INPUT PER RACER.
//
// This is the common ground the opponents and the network both stand on.
// Nothing in here reads a clock, a device or a frame: a tick is a
// pure function of the previous state and the inputs handed in, which is what lets every
// machine in a lockstep game run it and arrive at the same bits. `checksum()` is how they check.
//
// ⚠️ THE ORDER IS THE GAME'S. The race is ticked first (the original winds the countdown down
// before the vehicles move), then the cars one after the other in GRID order - car i meets cars
// 0..i-1 where they are after this tick and i+1.. where they were before it, exactly as the
// original's sequential loop does. Reordering either changes the result.
#pragma once

#include <cstdint>
#include <vector>

#include "race.h"
#include "replay.h"
#include "vehicle.h"

namespace bigred {

// The original's grid holds six (its `grid` line). Twelve is planned for the
// network game and needs a grid layout of its own; the session itself has no fixed limit.
constexpr int kGridRacers = 6;
// The most racers a race can hold: the network game's twelve. Places 6..11 are not in the
// data - grid_slot() extends the level's six (sim/race.h).
constexpr int kMaxRacers = 12;

struct RacerSetup {
    const VehicleTuning* tuning = nullptr;   // owned by the caller, must outlive the session
    int damage_option = 30;                  // the share of damage taken, 0..100
    // A GHOST: this racer is driven by a recording instead of an input (sim/replay.h). The
    // track stays owned by the caller. Its entry in tick()'s input array is ignored.
    const ReplayTrack* ghost = nullptr;
    // Where this racer starts instead of its grid place - the fly-by's helicopter, at the
    // level's `heli` point. Null = the grid.
    const RespawnPoint* start = nullptr;
};

class RaceSession {
public:
    RaceSession() = default;
    RaceSession(const RaceSession&) = delete;             // the cars point at each other
    RaceSession& operator=(const RaceSession&) = delete;

    // Everything a race needs. `terrain` and `world` stay owned by the caller. `origin` is where
    // a level without a `grid` line starts everyone (the map centre, in practice).
    void configure(const SimTerrain& terrain, SceneryState* world, std::vector<Gate> gates,
                   int laps, int countdown_ticks, std::vector<CheckLine> lines,
                   const StartingGrid& grid, const NitroReserve& nitros,
                   const std::vector<RacerSetup>& racers, const RespawnPoint& origin,
                   const ReplayRates& rates = ReplayRates());

    // THE RULES this race is played by (sim/race_rules.h): the deviations, the damage, the
    // rubber band, the end-of-race timings. Every car reads the session's own, so set them
    // before the first tick - the same on every machine of a network game.
    //
    // The original's RUBBER BAND (`rules.rubber_band`): every 16 ticks, every ghost's playback
    // rate steps 0x10 toward the level's `max` while the player leads, toward `min` when the
    // player is fifth or worse, toward `normal` in between, when the player is wrecked, or once
    // any racer - the player too - is on its last lap.
    RaceRules rules;
    // The original's two-body contact between cars (VehicleSim::two_body_contacts): both cars
    // take the reaction and share the separation. The original's modes 0x20/0x23 - set for a
    // race between several humans, off for one player against the ghosts. configure() sets it
    // from the field (two humans or more); set it afterwards to force either way.
    bool two_body_contacts = false;
    int32_t ghost_rate() const { return ghost_rate_; }
    bool is_ghost(int i) const { return ghosts_[static_cast<size_t>(i)].track != nullptr; }
    // A ghost whose recording has run out: it now stands where its last record left it.
    bool ghost_finished(int i) const {
        const GhostDriver& g = ghosts_[static_cast<size_t>(i)];
        if (!g.track || !g.track->valid()) return false;
        return g.time >= (static_cast<int32_t>(g.track->records.size()) << 12) - 1;
    }

    // ONE tick. `inputs` holds racer_count() entries, in grid order.
    void tick(const VehicleInput* inputs);

    int racer_count() const { return static_cast<int>(cars_.size()); }
    VehicleSim& car(int i) { return cars_[static_cast<size_t>(i)]; }
    const VehicleSim& car(int i) const { return cars_[static_cast<size_t>(i)]; }
    RaceState& race() { return race_; }
    const RaceState& race() const { return race_; }
    const RespawnPoint& start(int i) const { return starts_[static_cast<size_t>(i)]; }
    uint32_t ticks() const { return ticks_; }

    // OUT OF THE RACE: over the line, or wrecked `rules.wreck_out_ticks` ago - a wreck will
    // never cross the line and must not be waited for. Read off the car's own counter
    // (VehicleSim::wrecked_ticks), which travels in the resync snapshot.
    bool out_of_race(int i) const;
    // Ticks since racer `i` was destroyed, -1 while it runs.
    int32_t wrecked_for(int i) const {
        const VehicleSim& c = car(i);
        return c.damage < 0 ? c.wrecked_ticks : -1;
    }
    // THE FINISH COUNTDOWN (`rules.finish_countdown_ticks`): the tick the race ends on for
    // everyone, set on the tick more than half of the humans are home - the ones who left a
    // network game included, since their cars never will be and the countdown is what stops
    // the others waiting for them. 0 = not running. In the snapshot, so a resync that spans the
    // moment cannot leave two machines ending on different ticks.
    uint32_t finish_deadline() const { return finish_deadline_; }
    // True on the tick racer `i` was put back on the track - the camera re-plants on it.
    bool respawned(int i) const { return respawned_[static_cast<size_t>(i)] != 0; }

    // A 32-bit FNV-1a over everything the simulation carries from one tick to the next that a
    // desync would show in: every car's position, attitude, velocities, damage and boost, every
    // racer's progress, and the destruction state. Two machines that fed the same inputs must
    // agree on it at every tick; the first tick they do not is where they diverged.
    uint32_t checksum() const;

private:
    friend struct SnapshotAccess;   // sim/snapshot.cpp
    void link_field();

    std::vector<VehicleSim> cars_;
    std::vector<VehicleSim*> field_;
    std::vector<RespawnPoint> starts_;
    std::vector<uint8_t> respawned_;
    std::vector<int> respawn_gate_;   // the gate respawn_choice picked this tick, per racer
    std::vector<GhostDriver> ghosts_;
    ReplayRates rates_;
    int32_t ghost_rate_ = 0x200;
    uint32_t band_clock_ = 0;   // the original's timer count, in 14 ms ticks
    void rubber_band_step();
    RaceState race_;
    SceneryState* world_ = nullptr;
    uint32_t ticks_ = 0;
    uint32_t finish_deadline_ = 0;
    void finish_countdown_step();
};

} // namespace bigred
