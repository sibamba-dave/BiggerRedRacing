#include "vehicle_audio.h"

#include "../sim/vehicle.h"

namespace bigred {
namespace {

// The original's distance curve: full volume out to 0x2000 - 0x1ce0 = 800 world
// units, then a straight fade to silence at 0x2000 = 8192. Below 3/256 it stops the voice
// outright rather than mixing an inaudible one, which is also what keeps the 8-voice pool free.
constexpr int32_t kSilenceRange = 0x2000;
constexpr int32_t kFadeSpan = 0x1ce0;
constexpr int32_t kAudibleFloor = 3;

// The collision magnitude below which nothing is heard. It is the original's own damage
// threshold (`impact_magnitude > 0x5db`): under it a contact is a scrape,
// not a crash, and it neither damages the car nor deserves a bang.
constexpr int32_t kImpactFloor = 0x5db;
// How much "past the threshold" is a full-volume event. The original's own number, from
// its landing trigger: `if (excess > 10000) intensity = 256 else (excess<<8)/10000`.
// It is the only place the engine states what counts as a big impact, so it sets the scale for
// the collision bang too.
constexpr int32_t kIntensitySpan = 10000;
// How loud flattening a destructible prop is. ⚠️ OURS: the original passes whatever its caller
// happened to leave behind, which cannot be pinned down. Two thirds of full scale - a
// sheep should be clearly audible without competing with a wall.
constexpr int32_t kSplatIntensity = 170;

// Which `hit` entry a flattened prop plays. CONFIRMED in the original's scenery pass, whose two
// destructible cases are identical:
//
//     if (var > 0 && var < 4)  index = 0x1f + var;   // flat 32/33/34 = hit 4 / 5 / 6
//     else                     index = 0x1d;         // flat 29       = hit 1
//
// and that is exactly what the scripts' own comment says: "1 = splat with var = 0, 4,5,6 =
// splat objects with var = 1,2,3". `var` is MAP.DAT's offset-21 byte.
//
// ⚠️ THE VAR IS PER-LEVEL, NOT PER-SHAPE. On CITY4 the plants carry 0, the SHEEP 1, the cones 2
// and the signs 3 - so a BEETLE, whose script declares `hit 1 = splat`, `hit 4 = env sheep2`,
// `hit 5 = cone`, `hit 6 = sign`, gets four distinct sounds. On RIG3 the same SHEEP carries 4,
// which is out of range, so it falls back to the generic splat. Hard-coding index 4 for every
// destructible made grass, sheep and cones all bleat (user, 2026-09-21).
int hit_index_for_var(uint8_t var) {
    return (var >= 1 && var <= 3) ? (3 + static_cast<int>(var)) : 1;
}

int32_t volume_for(int32_t distance) {
    int32_t v = kSilenceRange - distance;
    if (v > kFadeSpan) v = kFadeSpan;
    if (v < 0) v = 0;
    return (v << 8) / kFadeSpan;
}

} // namespace

void VehicleAudio::configure(const VehicleTuning* tuning, SoundBank* bank, Mixer* mixer) {
    stop();
    tuning_ = tuning;
    bank_ = bank;
    mixer_ = mixer;
    last_land_ = -1;
    last_near_ground_ = 0;
}

void VehicleAudio::stop() {
    if (mixer_) {
        mixer_->stop(skid_.voice);
        mixer_->stop(ground_.voice);
        mixer_->stop(engine_.voice);
        mixer_->stop(nitro_.voice);
    }
    skid_ = Continuous();
    ground_ = Continuous();
    engine_ = Continuous();
    nitro_ = Continuous();
}

const FxSound* VehicleAudio::entry(FxCategory cat, int index) const {
    if (!tuning_ || index < 0 || index >= kFxIndexCount) return nullptr;
    const FxSound& e = tuning_->fx[static_cast<int>(cat)][index];
    return e.declared() ? &e : nullptr;
}

void VehicleAudio::drive(Continuous& c, FxCategory cat, int index, bool want, int32_t volume,
                         int32_t pan, int32_t rate_override, SlotDebug& dbg) {
    dbg.want = want;
    dbg.declared = entry(cat, index) != nullptr;
    dbg.playing = false;   // `starts` is cumulative and deliberately not cleared
    if (!mixer_ || !bank_) return;
    const FxSound* e = want ? entry(cat, index) : nullptr;
    // An undeclared entry is not a failure: plenty of vehicles simply have no sound for a given
    // surface, and the original plays nothing there either.
    const bool should = e != nullptr && volume >= kAudibleFloor;

    if (!should) {
        if (c.on) { mixer_->stop(c.voice); c.on = false; }
        c.sample_index = -1;   // the episode is over; the next rising edge starts a new one
        return;
    }
    // ⚠️ THE THIRD NUMBER OF THE LINE DECIDES WHETHER IT LOOPS, in every category. It used to
    // be overridden here - "the CATEGORY decides whether a sound is continuous, not the flag" -
    // on the reading that a `ground` line declaring a one-shot was an authoring slip. It is not
    // a slip. Printed across the roster by the bench's SOUND block, the flag is perfectly
    // consistent: 0 on everything that sustains (engine, `tur`, `drv_wat`, every `skid`) and 1
    // on everything that is an event (`notturb`, every `land`, `hit` and `check`) - AND on
    // `ground 3/4/5`, the rock, grass and sand rolling entries.
    //
    // Those three are one-shots because their samples are: `GRASS.WAV` is a one-second swish
    // that decays to silence, so looping it is a thump once a second for the rest of the lap.
    // And `GEN/DRV_ROCK.WAV` is **byte-for-byte `GEN/BANG3.WAV`**, the standard impact - same
    // MD5 - so looping THAT is the crash sound on repeat while you drive. Which is exactly the
    // report: "the standard impact sound plays in a loop, but only on the grass texture"
    // (user, 2026-09-21).
    const bool loop = !e->one_shot;
    // Already in the same episode on the same sample. This IS the original's
    // pair of tests: the surface changing is what forces a restart, because the surface is what
    // chose the sample, and otherwise the original posts the request only on the RISING EDGE
    // of the flag.
    if (c.sample_index == index) {
        if (c.on && mixer_->playing(c.voice)) {
            VoiceParams p;
            p.rate = rate_override > 0 ? rate_override : e->freq;
            p.volume = volume;
            p.pan = pan;
            mixer_->update(c.voice, p);
            dbg.playing = true;
            return;
        }
        // The voice is gone. A LOOPING one can only have been stolen by the mixer, so put it
        // back; a ONE-SHOT has simply finished, and restarting it is the machine-gun above.
        if (!loop) { c.on = false; return; }
    }
    if (c.on) mixer_->stop(c.voice);
    c.on = false;
    c.sample_index = index;     // the episode is claimed even if the sample is missing
    ++dbg.starts;
    const Sample* s = bank_->get(e->dir, e->name);
    if (!s) return;
    VoiceParams p;
    p.rate = rate_override > 0 ? rate_override : e->freq;
    p.volume = volume;
    p.pan = pan;
    c.voice = mixer_->play(*s, p, loop, e->loop_start, e->loop_end, cat == FxCategory::Engine);
    c.on = c.voice.valid();
    dbg.playing = c.on;
}

void VehicleAudio::fire(FxCategory cat, int index, int32_t volume, int32_t pan,
                        int32_t intensity, const char* label) {
    ++debug_.one_shots;
    debug_.last_event = label;
    // Dispatch on the first letter: cheap, and every label here starts with a distinct one.
    switch (label[0]) {
        case 'l': ++debug_.n_land; break;
        case 's': ++debug_.n_splat; break;
        case 'h': ++debug_.n_hit; break;
        case 'e': ++debug_.n_explode; break;
        default:  ++debug_.n_notturb; break;
    }
    if (!mixer_ || !bank_) return;
    const FxSound* e = entry(cat, index);
    if (!e || volume < kAudibleFloor) return;
    const Sample* s = bank_->get(e->dir, e->name);
    if (!s) return;
    VoiceParams p;
    p.rate = e->freq;
    // The original's own rule: an intensity at or above full scale plays at full
    // volume, and below it scales the distance volume down. That is what makes a scrape quieter
    // than a crash instead of every contact being a bang.
    p.volume = (intensity >= 256) ? volume : (intensity <= 0 ? 0 : intensity * volume / 256);
    p.pan = pan;
    if (p.volume < kAudibleFloor) return;
    if (p.volume > debug_.peak_one_shot) debug_.peak_one_shot = p.volume;
    mixer_->play(*s, p, /*loop=*/false, 0, -1, /*is_engine=*/false);
}

void VehicleAudio::on_car_contact(bool same_model, int32_t distance, int32_t pan) {
    const int32_t volume = volume_for(distance);
    if (volume < kAudibleFloor) return;
    fire(FxCategory::Hit, same_model ? 2 : 3, volume, pan, 256, "car");
}

void VehicleAudio::on_race_event(int check_index, int32_t distance, int32_t pan) {
    if (check_index < 0 || check_index >= kFxIndexCount) return;
    fire(FxCategory::Check, check_index, volume_for(distance), pan, 256, "check");
}

void VehicleAudio::on_tick(const VehicleSim& car, int32_t distance, int32_t pan) {
    if (!tuning_) return;
    const int32_t volume = volume_for(distance);
    const int land = car.land & 7;
    debug_.one_shots = 0;   // per tick; the n_* tallies below are cumulative
    debug_.volume = volume;
    debug_.land = land;
    const bool alive = car.damage >= 0;

    // --- the four continuous voices, FIRST ------------------------------------------------
    //
    // ⚠️ THE ORDER IS THE ORIGINAL'S, AND IT MATTERS. The original's per-car pass posts skid, ground, engine
    // and nitro, and only then drains the one-shot queue - so with eight voices the continuous
    // bed always has its four and impacts take what is left. Doing it the other way round is
    // what made the engine drop out under a burst of collisions (user, 2026-09-21: "le son du
    // moteur se coupe parfois quand un impact est trop fort").
    //
    // The engine's pitch rides the rev counter, the 0-250 rev, as in the original. That CONFIRMS what the sixth number of a `hnd fx` line is for -
    // the frequency at full intensity - and it is why an engine declared `11025,11000` (the
    // JEEP) does not change pitch at all. Its pitched variant exists in JEEP.TXT and is
    // COMMENTED OUT; the flat one is what ships.
    int32_t engine_rate = 0;
    if (const FxSound* e = entry(FxCategory::Engine, 0)) {
        engine_rate = e->freq;
        if (e->freq_full > 0) {
            int32_t rev = car.rev;
            if (rev < 0) rev = 0;
            if (rev > 250) rev = 250;
            engine_rate = e->freq + (e->freq_full - e->freq) * rev / 250;
        }
        // ⚠️ ÉCART ASSUMÉ, ours and not the original's: the rate is eased toward its target
        // instead of jumping to it. `rev` falls off a cliff at every gear change, and on a
        // BEETLE - which declares the widest range in the game, 12000 to 35000 - that is a
        // step of nearly an octave. The user heard exactly that: "comme si ça manquait de
        // transition entre les vitesses". A quarter of the gap per 28 ms tick settles in about
        // a tenth of a second, which reads as a gearbox rather than as a glitch.
        //
        // Set kEngineGlide to 1 to get the original's raw stepping back.
        constexpr int32_t kEngineGlide = 4;
        if (engine_rate_ <= 0) engine_rate_ = engine_rate;
        engine_rate_ += (engine_rate - engine_rate_) / kEngineGlide;
        engine_rate = engine_rate_;
    }
    drive(engine_, FxCategory::Engine, 0, alive, volume, pan, engine_rate, debug_.engine);
    drive(nitro_, FxCategory::Nitro, 0, car.boost_active, volume, pan, 0, debug_.nitro);
    drive(skid_, FxCategory::Skid, land, car.skidding, volume, pan, 0, debug_.skid);
    drive(ground_, FxCategory::Ground, land, car.moving, volume, pan, 0, debug_.ground);

    // --- then the one-shots ---------------------------------------------------------------
    //
    // ⚠️ EVERY ONE OF THESE CARRIES AN INTENSITY, and the intensity is the VOLUME. As in the
    // original: the queued value is compared with 256, and below it the sound plays at
    // `intensity * distance_volume / 256` - so a light knock is quiet and only a real one is
    // loud. Playing them all at full volume is what made impacts drown everything else.
    // THE EXPLOSION, on the tick the vehicle dies - `hnd fx hit 7` is `explode` on every
    // vehicle in the game (the original plays flat index 0x23 at 100% damage). Outside the
    // `alive` guard below, deliberately: this is the sound of no longer being alive.
    if (car.just_destroyed && volume >= kAudibleFloor) {
        fire(FxCategory::Hit, 7, volume, pan, 256, "explode");
    }
    // ⚠️ NOT GATED ON `alive`, and that is confirmed, not a guess. The original drains the
    // one-shot queue under one condition and one only - `if (2 < volume)`. The destroyed flag
    // gates the ENGINE voice and nothing else, so in the original a wrecked car still
    // bangs, still splats a sheep and still thuds on landing; it simply does so in silence
    // where its engine used to be.
    //
    // This was the user's own question, and it is the answer to the other half of their report:
    // impacts DO sound on a destroyed car, so "sometimes the sheep make no noise" was not a
    // trigger that fails at random. The car was already wrecked - see the damage rate in
    // VehicleSim::hit_scenery - and the guard that used to be here took every one-shot with it.
    if (volume >= kAudibleFloor) {
        // LANDING. ⚠️ NOT "a wheel touched the ground": that happens on every ripple of the
        // terrain, and firing on it put a thud under the smallest hop (user, 2026-09-21: "le
        // moindre petit saut provoque le son"). The original's gate is:
        //
        //     if (impact / 2 > 3999) play(land[surface], volume = impact / 2 - 4000)
        //
        // so the touchdown speed has to exceed 8000 before anything is heard at all, and the
        // volume is how far past that it went.
        // ⚠️ AND THE EXCESS IS NOT THE INTENSITY - it has to be mapped onto the 0..256 scale
        // first. The original does this too, though it is easy to miss:
        //
        //     excess    = impact/2 - 4000
        //     intensity = (excess > 10000) ? 256 : (excess << 8) / 10000
        //
        // so a touchdown ramps from silent to full over 10 000 units of excess. Passing the raw
        // excess made every landing full volume the moment it passed 256 - a 6 % margin over
        // the threshold - which is why the smallest hop landed like a truck.
        if (car.landing_impact > 0) {
            const int32_t excess = car.landing_impact / 2 - 4000;
            if (excess > 0) {
                const int32_t intensity = (excess > 10000) ? 256 : (excess << 8) / 10000;
                fire(FxCategory::Land, land, volume, pan, intensity, "land");
            }
        }
        // IMPACT. Two quite different events share this category:
        //
        //  * a SOLID obstacle pushes back, and `impact_magnitude` says how hard - index 0;
        //  * a DESTRUCTIBLE prop does not push back at all (see VehicleSim::kDestructiblesPushBack),
        //    so its magnitude is zero. ⚠️ Gating on the magnitude therefore made flattening a
        //    sheep SILENT. What identifies that event is the wreck itself, and WHICH sound it
        //    makes comes from the prop's own `var` - see hit_index_for_var.
        if (car.scenery_wrecked >= 0) {
            fire(FxCategory::Hit, hit_index_for_var(car.scenery_wrecked_var), volume, pan,
                 kSplatIntensity, "splat");
        } else if (car.scenery_hit && car.vehicle_hit < 0 && car.impact_magnitude > kImpactFloor) {
            // ⚠️ `vehicle_hit < 0`: a contact with another CAR raises the same shake flag, but
            // the original's `hit 0` is queued by the scenery pass alone. A car
            // contact sounds `hit 2`/`hit 3`, on the other car - RaceScreen does that.
            // Scaled the same way a landing is, and over the same span: the original's own
            // 10 000-unit ramp to full volume. ⚠️ The intensity itself is OURS -
            // the original queues an impact with whatever its caller happened to leave, which for
            // a collision is nothing it set - but a FLAT intensity is certainly wrong, because
            // then a graze and a head-on crash are the same bang.
            const int32_t excess = car.impact_magnitude - kImpactFloor;
            const int32_t intensity = (excess > kIntensitySpan) ? 256
                                                                : (excess << 8) / kIntensitySpan;
            fire(FxCategory::Hit, 0, volume, pan, intensity, "hit");
        }
        // The boost key on an empty tank: the script calls the sample `notturb`.
        if (car.nitro_refused) fire(FxCategory::Nitro, 1, volume, pan, 256, "notturb");
    }

    last_land_ = land;
    last_near_ground_ = car.near_ground;
}

} // namespace bigred
