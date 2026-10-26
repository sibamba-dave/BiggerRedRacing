// A whole race, frozen into bytes and thawed back - what the network game's RESYNC sends.
//
//
// A lockstep game that has desynchronised cannot be talked back into agreement: the two
// machines would have to find and undo the difference. Instead the host freezes its own race at
// an agreed tick and every other machine replaces its race with it. Everything the simulation
// carries from one tick to the next is in here - the cars, the race, the ghosts, the destruction
// state, the session's own counters - and nothing the renderer owns.
//
// ⚠️ SAME BUILD ONLY. The cars travel as raw bytes (VehicleSim is trivially copyable, checked at
// compile time), so both machines must run the same executable - which the lockstep game needs
// anyway. The header carries the struct sizes, and a mismatch refuses the snapshot rather than
// loading garbage.
//
// ⚠️ A NEW FIELD in RacerProgress, RaceState, GhostDriver or RaceSession must be added to
// snapshot.cpp by hand. net_bench's resync test is what catches a forgotten one: a thawed race
// has to stay identical to the original, tick after tick.
#pragma once

#include <cstdint>
#include <vector>

namespace bigred {

class RaceSession;

std::vector<uint8_t> save_race(const RaceSession& session);
// False, and the session untouched, when the bytes are not a snapshot of a race shaped like
// this one (another build, another racer count, another level).
bool load_race(RaceSession& session, const uint8_t* data, size_t size);

} // namespace bigred
