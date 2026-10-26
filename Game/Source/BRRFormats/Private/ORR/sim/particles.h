// The ground-effect particle field: wheel dust, spray and impact sparks.
//
// This is simulation, not rendering:
// a particle is born on a tick and dies on a tick, in integers, so two machines running the same
// inputs hold the same field - the lockstep rule.
//
// The original's shape, ported as-is:
//   * a FIXED pool of 100 slots walked as a ring (stride 60 bytes). When it
//     fills, the oldest is overwritten - there is no allocation and no growth;
//   * a particle DOES NOT MOVE. It is born where a wheel touches the ground, plays its flipbook
//     in place and vanishes. The original's update advances only the frame counter;
//   * what it plays comes from the environment's `spot` lines, so the surface under the wheel
//     decides the effect: dust on the track, spray in the water (see SpotEffect).
#pragma once

#include <cstdint>
#include <vector>

#include "formats/level_script.h"

namespace bigred {

class VehicleSim;

// The original's own bound.
constexpr int kParticleSlots = 100;

// One live particle. `type` doubles as the liveness flag, exactly as the original's does.
struct Particle {
    int32_t x = 0, y = 0, z = 0;       // world units, where it was born - it stays there
    int32_t terrain_pitch = 0;         // the vehicle's terrain pitch/roll at the moment of birth,
    int32_t terrain_roll = 0;          // copied through so the puff can lie on the slope
    int type = 0;                      // 0 = free slot; else the `spot` line's type (1/2/3)
    int kind = -1;                     // which SpotEffect it is playing
    int32_t held = 0;                  // ticks this frame has been showing
    int32_t frame = 0;                 // flipbook frame = the shape's variant byte

    bool alive() const { return type != 0; }
};

// The pool, plus the emission rules. One per race.
class ParticleField {
public:
    // The environment's `spot` table. Kept by value: it is five ints and a name per line.
    void configure(std::vector<SpotEffect> spots);
    void reset();

    const std::vector<SpotEffect>& spots() const { return spots_; }
    const std::vector<Particle>& slots() const { return slots_; }
    int live_count() const;

    // ONE 14 ms GAME tick of the field: advance each frame counter, retire what has run past its
    // last frame. ⚠️ The game ticks this at 14 ms while the physics runs at 28 ms
    // (level_script.h), so a caller stepping the physics calls this TWICE per step.
    void tick();

    // The emitter, once per 28 ms PHYSICS tick. `tick_index` is the original's timer-tick
    // counter: skid dust is gated on its low two bits, which is what makes a
    // sliding tyre smoke half as often as a rolling one.
    // ⚠️ Takes the vehicle by NON-const reference: a spark consumes the impact flag, exactly
    // as the original clears its impact flag the moment it spawns one. Without that a single collision
    // sprays a new shower every tick for as long as the car stays against the obstacle.
    void emit(VehicleSim& car, uint32_t tick_index);

private:
    // The original's ring allocator.
    Particle& allocate();
    void spawn_wheel(const VehicleSim& car, int kind, int corner);

    std::vector<SpotEffect> spots_;
    std::vector<Particle> slots_;
    int head_ = 0;   // where the next particle is written
    // The original's 1-in-4 visual variant draw (`rand % 4`), which starts a puff part-way into
    // its filmstrip so neighbouring wheels do not pulse in lockstep. Deterministic on purpose:
    // a shared seed advanced only by the simulation keeps two peers bit-identical.
    uint32_t rng_ = 0x13579bdfu;
    uint32_t next_random();
};

} // namespace bigred
