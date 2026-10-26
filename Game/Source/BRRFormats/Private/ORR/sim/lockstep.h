// The input side of a lockstep game, with no transport in it.
//
// In a lockstep race the only thing that crosses the network is each player's controls for each
// tick; every machine runs the whole RaceSession itself.
// What that needs, independently of how the bytes travel:
//
//   * a control state that fits in ONE BYTE - the six logical actions are
//     six bits, so a whole tick of a six-player race is six bytes;
//   * an INPUT DELAY: what a player presses now is scheduled `delay` ticks ahead, so it has that
//     long to reach the others before anyone needs it. The first `delay` ticks of the race are
//     empty for everyone, which is harmless - the countdown runs for seconds;
//   * a window that says, for a given tick, whether every player's input is in. The session may
//     only be ticked when it is. A machine that is ahead simply waits: that is the lockstep.
//
// ENet, the lobby and the discovery beacon are not here and do not need to be to test this: the
// bench feeds two sessions through one of these and checks they never disagree.
#pragma once

#include <cstdint>
#include <vector>

#include "vehicle.h"

namespace bigred {

// Six bits, in a fixed order. Bits 6-7 are always 0 - a byte with either set is corrupt.
uint8_t pack_input(const VehicleInput& in);
VehicleInput unpack_input(uint8_t bits);

class LockstepInputs {
public:
    // How far apart the fastest and slowest machine may run, in ticks. 256 is 7 seconds at
    // 28 ms - far beyond any delay worth playing with.
    static constexpr uint32_t kWindow = 256;

    // Ticks are numbered from 1, like RaceSession::ticks() after its first tick. The first
    // `delay` ticks are filled with "nothing pressed" for every player.
    void configure(int players, int delay_ticks);
    int players() const { return players_; }
    int delay() const { return delay_; }

    // Records `player`'s input for `tick`. A repeat of a tick already known is ignored (a
    // resent packet); returns false if `tick` is outside the window, which means the two ends
    // have drifted further apart than the game can absorb.
    bool submit(int player, uint32_t tick, uint8_t bits);
    // The tick the LOCAL player's input sampled now belongs to, given the tick about to be run.
    uint32_t schedule(uint32_t next_tick) const { return next_tick + static_cast<uint32_t>(delay_); }

    bool ready(uint32_t tick) const;
    // The first player whose input for `tick` is not in yet, or -1 when ready(tick). Display
    // only: says whom a waiting machine is waiting for.
    int missing(uint32_t tick) const;
    // Every player's input for `tick`, in player order. Only meaningful when ready(tick).
    void inputs_for(uint32_t tick, VehicleInput* out) const;
    // Frees the slots of every tick before `tick`, once it has been run.
    void release_before(uint32_t tick);

private:
    size_t slot(uint32_t tick, int player) const {
        return static_cast<size_t>(tick % kWindow) * static_cast<size_t>(players_) +
               static_cast<size_t>(player);
    }
    int players_ = 0;
    int delay_ = 0;
    uint32_t base_ = 1;                 // the oldest tick still held
    std::vector<uint16_t> slots_;       // 0x100 | bits when known, 0 when not
    std::vector<uint32_t> slot_tick_;   // which tick a window row currently holds
};

// WHEN a network machine may run its next tick - the pacing half of RaceScreen::step_network,
// kept here so net_bench's latency model runs exactly the game's rule. It never touches what a
// tick computes. `banked` is the time owed to the simulation, in ticks.
//   Run   - every input for `next` is in, and either those for `next + 1` too or the machine
//           is two ticks late: go.
//   Hold  - `next` could run, but ONE TICK IS KEPT IN RESERVE (a jitter buffer): run it a
//           little later rather than the instant its last packet lands.
//   Wait  - an input for `next` is missing.
enum class LockstepGate { Run, Hold, Wait };
LockstepGate lockstep_gate(const LockstepInputs& lk, uint32_t next, double banked_ticks);
// At most this many ticks in one frame; and a machine that waits keeps at most this much time
// owed, so it does not race through a pile of it later and become the one everybody waits for.
constexpr int kLockstepMaxTicksPerFrame = 8;
constexpr double kLockstepWaitBankTicks = 4.0;

} // namespace bigred
