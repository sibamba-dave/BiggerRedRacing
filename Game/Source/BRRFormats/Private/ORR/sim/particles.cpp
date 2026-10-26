#include "particles.h"

#include "vehicle.h"

namespace bigred {

void ParticleField::configure(std::vector<SpotEffect> spots) {
    spots_ = std::move(spots);
    reset();
}

void ParticleField::reset() {
    slots_.assign(kParticleSlots, Particle{});
    head_ = 0;
    rng_ = 0x13579bdfu;
}

int ParticleField::live_count() const {
    int n = 0;
    for (const Particle& p : slots_) if (p.alive()) ++n;
    return n;
}

uint32_t ParticleField::next_random() {
    // A plain LCG. The original's generator is a different one, and it does not matter: the draw
    // picks a starting frame out of four, so what it has to be is DETERMINISTIC, not identical.
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_ >> 16;
}

Particle& ParticleField::allocate() {
    Particle& p = slots_[static_cast<size_t>(head_)];
    head_ = (head_ + 1) % kParticleSlots;
    p = Particle{};
    return p;
}

void ParticleField::tick() {
    for (Particle& p : slots_) {
        if (!p.alive()) continue;
        const SpotEffect& s = spots_[static_cast<size_t>(p.kind)];
        if (++p.held < s.speed) continue;
        p.held = 0;
        ++p.frame;
        // ⚠️ The test is `frame_count < frame`, so the LAST frame does show: a 12-frame effect
        // runs frames 0..12 inclusive. Writing `>=` here cuts a frame off every effect.
        if (s.frame_count < p.frame) p.type = 0;
    }
}

void ParticleField::spawn_wheel(const VehicleSim& car, int kind, int corner) {
    const SpotEffect& s = spots_[static_cast<size_t>(kind)];
    Particle& p = allocate();
    p.x = car.corner_x[corner];
    p.y = car.corner_y[corner];
    p.z = car.corner_z[corner];
    p.terrain_pitch = car.terrain_pitch;
    p.terrain_roll = car.terrain_roll;
    p.type = s.type;
    p.kind = kind;
    // The draw that starts a puff part-way in. It costs the particle that many frames of life,
    // because death is "the frame counter passed frame_count", not "it has lived N ticks".
    p.frame = static_cast<int32_t>(next_random() % 4u);
}

void ParticleField::emit(VehicleSim& car, uint32_t tick_index) {
    if (spots_.empty()) return;

    // Which of a pair of corners gets the particle: the first one that is touching the ground.
    // The original writes this as a preference, not a loop, and it never emits from both - two
    // puffs per tick per vehicle is the whole budget. The PREFERENCE flips with the direction of
    // the slide so the nearer wheel is drawn last, which mattered to a painter's algorithm with
    // no depth buffer; it costs us nothing to keep it.
    const uint16_t on_ground = car.near_ground;
    auto first_on_ground = [&](int a, int b) {
        if (on_ground & (1u << a)) return a;
        if (on_ground & (1u << b)) return b;
        return -1;
    };

    for (size_t k = 0; k < spots_.size(); ++k) {
        const SpotEffect& s = spots_[k];
        const int kind = static_cast<int>(k);
        if (s.type == 1) {
            // Skidding: the vehicle's skid flag, which is |vlat| > 5000 measured on the ground -
            // see VehicleSim::skidding. ⚠️ This used to read `mode`, and `mode` is fed by the
            // YAW-RATE limiter, so dust flew on every corner whether the car was sliding or not.
            if (!car.skidding) continue;
            if (car.land != s.land) continue;
            // Half the rate of the driving dust - the original's own `& 3` on the timer tick.
            if ((tick_index & 1u) != 0) continue;
            const bool slide_left = car.vlat < 1;
            const int a = slide_left ? first_on_ground(3, 0) : first_on_ground(0, 3);
            const int b = slide_left ? first_on_ground(2, 1) : first_on_ground(1, 2);
            if (a >= 0) spawn_wheel(car, kind, a);
            if (b >= 0) spawn_wheel(car, kind, b);
        } else if (s.type == 2) {
            // Driving. Needs real road speed - below the threshold a car raises nothing.
            if (car.land != s.land) continue;
            if (car.vfwd > 3000) {
                const int a = first_on_ground(0, 1);
                const int b = first_on_ground(3, 2);
                if (a >= 0) spawn_wheel(car, kind, a);
                if (b >= 0) spawn_wheel(car, kind, b);
            } else if (car.vfwd < -3000) {
                const int a = first_on_ground(1, 0);
                const int b = first_on_ground(2, 3);
                if (a >= 0) spawn_wheel(car, kind, a);
                if (b >= 0) spawn_wheel(car, kind, b);
            }
        } else if (s.type == 3) {
            // Impact sparks. No surface test at all - that is why `SPARK_FX` is declared with
            // land -1: the field is never read for this type. One particle, at the contact
            // point, and the flag is consumed so a single collision cannot spray every tick.
            if (!car.spark_pending) continue;
            Particle& p = allocate();
            p.x = car.spark_x;
            p.y = car.spark_y;
            p.z = car.spark_z;
            p.type = s.type;
            p.kind = kind;
            p.frame = 0; // sparks always start on frame 0 - no random draw here
            car.spark_pending = false;
        }
    }
}

} // namespace bigred
