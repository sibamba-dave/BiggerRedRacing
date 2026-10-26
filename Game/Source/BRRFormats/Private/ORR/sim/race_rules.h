// THE RULES OF A RACE as the simulation plays them: the deliberate deviations and their values,
// the damage an impact costs, the ghosts' rubber band, and the end-of-race timings. Everything
// here changes what the simulation computes, so every machine of a network game must hold the
// same: the host's travels in the race's start message (net/net_protocol.h) and in every resync
// snapshot (sim/snapshot.h), and the session hands it to its cars (RaceSession::rules).
//
// Out of SimTerrain since 2026-09-28 (code review, item A3): the
// terrain is the ground, these are rules. A "fun" rule - gravity, damage, speed - is a new field
// here, a new word in to_words/from_words, and a protocol bump.
//
#pragma once

#include <cstdint>

namespace bigred {

struct RaceRules {
    // DELIBERATE DEVIATION 3 on or off - the airborne cap on the body's angular rates. On in
    // every race; `-NOAIRCAP` turns it off, for the user to compare the pitch recovery with the
    // original's.
    bool air_rate_cap = true;
    // ...and its value, in BAM24 per tick. 0x10000 since 2026-09-27 (it was 0x6000, `cogs[5]`):
    // with deviation 2 retired, that is the value whose jumps match the original's recorded
    // pitch without flipping any more than before - see the spec. `-AIRCAP=N` / BENCH_AIRCAP.
    int32_t air_rate_cap_value = 0x10000;
    // ...the same cap for the ROLL rate, on its own since 2026-09-27: only the PITCH was matched
    // against the original's ghosts when the cap went to 0x10000, and the roll followed it
    // unmeasured - a car launched off a banked slope then rolled 67 degrees in one flight (the
    // user's autodrive race, `net_bench - DIRT1 flips`). BENCH_AIRROLL.
    int32_t air_roll_cap_value = 0x10000;
    // Part of DELIBERATE DEVIATION 3, since 2026-09-27: in the air, each rate loses 1/2^N of
    // itself per tick, so a rotation taken at the take-off fades instead of turning the body at
    // one speed for a whole 1.4 s flight - which rolled cars over on DIRT1's long jumps (16
    // rollovers over 11 levels -> 5 at N = 7, and the ghosts' jumps matched slightly BETTER).
    // 7 chosen by the user in game. 0 = off. `-AIRDAMP=N` / BENCH_AIRDAMP; BENCH_AIRDAMP_ROLL
    // for the roll alone (0 = the same as the pitch).
    int32_t air_damp_shift = 7, air_roll_damp_shift = 0;
    // Former DELIBERATE DEVIATION 2 - the righting torque also reading the tolerance mask.
    // RETIRED 2026-09-27 (false: the original's righting torque reads the penetration mask alone). It piled
    // up pitch rate before every take-off. `-DEV2` / BENCH_DEV2 bring it back to compare.
    bool righting_tolerance = false;
    // An impact's damage is its magnitude shifted right by this - see
    // VehicleSim::kImpactDamageShift, the deviation it carries (the original's is 8).
    int32_t impact_damage_shift = 12;
    // The original's RUBBER BAND on the ghosts' playback rate - RaceSession::rubber_band_step.
    // Off in the original only with `-NOSPEED`; off here on a fly-by, which has
    // no human to pace them against.
    bool rubber_band = true;
    // A WRECK is out of the race this many ticks after it died, and is no longer waited for:
    // 350 timer ticks in a race alone, 105 in a network one, as in the original.
    // In 28 ms ticks.
    uint32_t wreck_out_ticks = 175;
    // THE FINISH COUNTDOWN of a network race: once more than half of the humans are home, the
    // race ends this many ticks later for everyone, finished or not. 0 = none (a race alone).
    uint32_t finish_countdown_ticks = 0;
    // THE SPEED CLASS, "Mario Kart CC" (user, 2026-09-30) - ours, not the original's: every
    // engine's gear table is stretched to this percentage of its speed and thrust
    // (VehicleSim::run_engine), and the ghosts are played back that much faster, on top of
    // their rubber band (RaceSession::tick). 100 = the original. See kSpeedClasses.
    int32_t speed_percent = 100;

    // A network race's: the same, with the network's end-of-race timings.
    static RaceRules network() {
        RaceRules r;
        r.wreck_out_ticks = 53;
        r.finish_countdown_ticks = 1071;   // 30 s
        return r;
    }

    // As fixed-size words, for the start message and the snapshot - the ORDER is the format.
    static constexpr int kWords = 11;
    void to_words(int32_t w[kWords]) const {
        w[0] = air_rate_cap ? 1 : 0;
        w[1] = air_rate_cap_value;
        w[2] = air_roll_cap_value;
        w[3] = air_damp_shift;
        w[4] = air_roll_damp_shift;
        w[5] = righting_tolerance ? 1 : 0;
        w[6] = impact_damage_shift;
        w[7] = rubber_band ? 1 : 0;
        w[8] = static_cast<int32_t>(wreck_out_ticks);
        w[9] = static_cast<int32_t>(finish_countdown_ticks);
        w[10] = speed_percent;
    }
    // The shifts are bounded: they come off the network, and a shift past 31 is undefined.
    void from_words(const int32_t w[kWords]) {
        const auto shift = [](int32_t v, int32_t hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
        air_rate_cap = w[0] != 0;
        air_rate_cap_value = w[1];
        air_roll_cap_value = w[2];
        air_damp_shift = shift(w[3], 15);
        air_roll_damp_shift = shift(w[4], 15);
        righting_tolerance = w[5] != 0;
        impact_damage_shift = shift(w[6], 30);
        rubber_band = w[7] != 0;
        wreck_out_ticks = static_cast<uint32_t>(w[8]);
        finish_countdown_ticks = static_cast<uint32_t>(w[9]);
        speed_percent = w[10] < 25 ? 25 : (w[10] > 400 ? 400 : w[10]);
    }
};

// The classes the menus offer, slowest first: the name shown and RaceRules::speed_percent.
// 100CC is the original game.
struct SpeedClass {
    const char* name;
    int32_t percent;
};
constexpr SpeedClass kSpeedClasses[] = {
    {"50CC", 75}, {"100CC", 100}, {"150CC", 150}, {"200CC", 200}};
constexpr int kSpeedClassCount = 4;

} // namespace bigred
