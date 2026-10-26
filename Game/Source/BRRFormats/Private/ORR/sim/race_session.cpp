#include "race_session.h"

namespace bigred {

void RaceSession::configure(const SimTerrain& terrain, SceneryState* world, std::vector<Gate> gates,
                            int laps, int countdown_ticks, std::vector<CheckLine> lines,
                            const StartingGrid& grid, const NitroReserve& nitros,
                            const std::vector<RacerSetup>& racers, const RespawnPoint& origin,
                            const ReplayRates& rates) {
    const int n = racers.empty() ? 1 : static_cast<int>(racers.size());
    world_ = world;
    ticks_ = 0;
    finish_deadline_ = 0;
    race_.configure(std::move(gates), laps, countdown_ticks, n, std::move(lines));
    cars_.assign(static_cast<size_t>(n), VehicleSim());
    starts_.assign(static_cast<size_t>(n), origin);
    respawned_.assign(static_cast<size_t>(n), 0);
    respawn_gate_.assign(static_cast<size_t>(n), -1);
    ghosts_.assign(static_cast<size_t>(n), GhostDriver());
    rates_ = rates;
    ghost_rate_ = 0x200;
    band_clock_ = 0;
    for (int i = 0; i < n; ++i) {
        const RacerSetup& r = racers.empty() ? RacerSetup() : racers[static_cast<size_t>(i)];
        if (grid.present) starts_[static_cast<size_t>(i)] = race_.start_of(grid, i);
        if (r.start) starts_[static_cast<size_t>(i)] = *r.start;
        VehicleSim& c = cars_[static_cast<size_t>(i)];
        if (!r.tuning) {                      // nobody in this slot: never an obstacle
            c.active = false;
            continue;
        }
        c.reset(*r.tuning, terrain, rules, starts_[static_cast<size_t>(i)], nitros);
        // ⚠️ AFTER reset(), which starts with `*this = VehicleSim()` and would wipe both.
        c.set_damage_option(r.damage_option);
        c.place_on_grid();
        c.world = world_;
        // A ghost starts from the pose the grid just gave it, as in the original.
        if (r.ghost && r.ghost->valid()) ghosts_[static_cast<size_t>(i)].start(*r.ghost, c);
    }
    // Two humans or more: the two-body contact of the original's modes 0x20/0x23. Derived from
    // the field rather than asked for, so every machine of a network game picks the same.
    int humans = 0;
    for (int i = 0; i < n; ++i)
        if (cars_[static_cast<size_t>(i)].active && !ghosts_[static_cast<size_t>(i)].track) ++humans;
    two_body_contacts = humans >= 2;
    link_field();
}

void RaceSession::link_field() {
    // Re-pointed every tick rather than once: the vector may have moved since, and a reset()
    // wipes the car's own copy.
    field_.resize(cars_.size());
    for (size_t i = 0; i < cars_.size(); ++i) field_[i] = &cars_[i];
    for (size_t i = 0; i < cars_.size(); ++i) {
        cars_[i].field = field_.data();
        cars_[i].field_count = static_cast<int>(cars_.size());
        cars_[i].field_self = static_cast<int>(i);
        cars_[i].two_body_contacts = two_body_contacts;
        cars_[i].rules = &rules;
        cars_[i].replayed = ghosts_[i].track != nullptr;
    }
}

void RaceSession::tick(const VehicleInput* inputs) {
    ++ticks_;
    link_field();
    // The race first, exactly as the original orders it: it winds the countdown down
    // and only then runs the per-vehicle update.
    race_.tick(field_.data(), racer_count());
    for (int i = 0; i < racer_count(); ++i) {
        VehicleSim& c = cars_[static_cast<size_t>(i)];
        respawned_[static_cast<size_t>(i)] = 0;
        if (!c.active) continue;
        // Where ALT puts the car back: the middle of the last gate it reached, moved back through
        // the gate while another car sits on the spot, as in the original. Refreshed
        // every tick so the key is instant; the others are where they are at this car's turn,
        // which is where the original finds them too.
        {
            struct Ctx {
                const RaceSession* s;
                int self;
            } ctx{this, i};
            auto taken = [](const RespawnPoint& at, const void* p) {
                const Ctx& k = *static_cast<const Ctx*>(p);
                const VehicleSim& me = k.s->cars_[static_cast<size_t>(k.self)];
                // The original asks the scenery first, then every other car.
                if (me.terrain && me.tune &&
                    me.terrain->spot_blocked(at.x, at.z, at.yaw_bam16, me.tune->box))
                    return true;
                for (int j = 0; j < k.s->racer_count(); ++j)
                    if (j != k.self &&
                        k.s->cars_[static_cast<size_t>(j)].blocks_spot(*me.tune, at.x, at.z, at.yaw_bam16))
                        return true;
                return false;
            };
            const RaceState::RespawnChoice pick =
                race_.respawn_choice(i, starts_[static_cast<size_t>(i)], taken, &ctx);
            c.respawn = pick.at;
            respawn_gate_[static_cast<size_t>(i)] = pick.gate;
        }
        // ⚠️ THE PHYSICS DOES NOT RUN UNTIL THE LIGHTS GO OUT - the original guards the whole
        // vehicle update with the countdown having run out.
        if (!race_.started()) continue;
        // As in the original, a ghost is played back, not simulated - no input, no collision pass of
        // its own. It is still an obstacle to the others, which find it in the field.
        if (ghosts_[static_cast<size_t>(i)].track) {
            // THE SPEED CLASS (RaceRules::speed_percent): the rubber band runs as in the
            // original, and its rate is played that much faster - the ghosts keep pace with
            // engines stretched the same way (VehicleSim::run_engine).
            ghosts_[static_cast<size_t>(i)].rate =
                rules.speed_percent == 100 ? ghost_rate_
                                           : fx::muldiv(ghost_rate_, rules.speed_percent, 100);
            ghosts_[static_cast<size_t>(i)].tick(c);
            c.dress_replayed();
            continue;
        }
        const uint32_t before = c.respawns;
        c.step(inputs ? inputs[i] : VehicleInput());
        if (c.respawns != before) {
            respawned_[static_cast<size_t>(i)] = 1;
            // The ring has to know, or the trip back from a gate already passed reads as
            // driving the wrong way - see RaceState::on_respawned.
            race_.on_respawned(i, respawn_gate_[static_cast<size_t>(i)]);
        }
    }
    if (race_.started()) rubber_band_step();
    finish_countdown_step();
}

bool RaceSession::out_of_race(int i) const {
    if (race_.progress(i).finished) return true;
    const int32_t since = wrecked_for(i);
    return since >= 0 && static_cast<uint32_t>(since) >= rules.wreck_out_ticks;
}

void RaceSession::finish_countdown_step() {
    if (rules.finish_countdown_ticks == 0 || finish_deadline_ != 0) return;
    size_t humans = 0, home = 0;
    for (int i = 0; i < racer_count(); ++i) {
        if (!cars_[static_cast<size_t>(i)].active || ghosts_[static_cast<size_t>(i)].track) continue;
        ++humans;
        if (race_.progress(i).finished) ++home;
    }
    if (humans > 0 && home * 2 > humans) finish_deadline_ = ticks_ + rules.finish_countdown_ticks;
}

void RaceSession::rubber_band_step() {
    // The original's clock counts the 14 ms timer ticks: two per tick of ours, so `& 0x1f` fires every
    // sixteen of them - 448 ms.
    band_clock_ += 2;
    if (!rules.rubber_band || (band_clock_ & 0x1f) != 0) return;
    int player = -1;
    for (int i = 0; i < racer_count() && player < 0; ++i)
        if (!ghosts_[static_cast<size_t>(i)].track && cars_[static_cast<size_t>(i)].active) player = i;
    if (player < 0) return;
    // The place, as the original's placing writes it: 0 for the leader.
    const int place0 = race_.progress(player).place - 1;
    int mode = (place0 == 0) ? 1 : 0;
    if (place0 != 0 && place0 < 3) mode = 3;
    if (place0 > 3) mode = 2;
    if (cars_[static_cast<size_t>(player)].damage < 0) mode = 3;
    // ANY car on its last lap, THE PLAYER INCLUDED: the original walks all seven slots, its own
    // among them (each slot says whether a car is in it). Leaving the player out kept the ghosts
    // at the top rate through a leader's whole last lap - "they look much faster than me"
    // (user, 2026-09-28).
    for (int i = 0; i < racer_count(); ++i)
        if (cars_[static_cast<size_t>(i)].active && race_.progress(i).lap >= race_.laps() - 1)
            mode = 3;
    // Only the first rate is compared and all of them move together.
    switch (mode) {
    case 1:
        if (ghost_rate_ < rates_.max) ghost_rate_ += 0x10;
        break;
    case 2:
        if (rates_.min < ghost_rate_) ghost_rate_ -= 0x10;
        break;
    case 3:
        if (ghost_rate_ < rates_.normal) ghost_rate_ += 0x10;
        if (rates_.normal < ghost_rate_) ghost_rate_ -= 0x10;
        break;
    default:
        break;
    }
}

namespace {

struct Fnv {
    uint32_t h = 2166136261u;
    void u32(uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xFFu;
            h *= 16777619u;
        }
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
};

} // namespace

uint32_t RaceSession::checksum() const {
    Fnv f;
    f.u32(ticks_);
    f.i32(ghost_rate_);
    f.u32(race_.race_ticks());
    for (const VehicleSim& c : cars_) {
        f.i32(c.active ? 1 : 0);
        f.i32(c.px); f.i32(c.py); f.i32(c.pz);
        f.i32(c.pitch); f.i32(c.yaw); f.i32(c.roll);
        f.i32(c.pitch_rate); f.i32(c.yaw_rate); f.i32(c.roll_rate);
        f.i32(c.speed); f.i32(c.travel); f.i32(c.vy); f.i32(c.vfwd); f.i32(c.vlat);
        f.i32(c.damage); f.i32(c.nitro_fuel); f.i32(c.boost_timer);
        f.i32(c.gear); f.i32(c.rev);
        f.u32(c.contact); f.u32(c.near_ground);
        f.u32(c.respawns);
    }
    for (int i = 0; i < race_.racer_count(); ++i) {
        const RacerProgress& p = race_.progress(i);
        f.i32(p.gate); f.i32(p.reached); f.i32(p.lap); f.i32(p.place);
        f.u32(p.last_check); f.i32(p.finished ? 1 : 0);
    }
    if (world_) {
        f.u32(static_cast<uint32_t>(world_->animating));
        for (size_t i = 0; i < world_->wrecked.size(); ++i)
            if (world_->wrecked[i]) f.u32(static_cast<uint32_t>(i));
    }
    return f.h;
}

} // namespace bigred
