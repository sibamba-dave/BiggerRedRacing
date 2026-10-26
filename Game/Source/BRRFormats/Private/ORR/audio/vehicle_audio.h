// Turning one vehicle's simulation state into sound.
//
// This is a faithful port of the original's per-vehicle sound routine, which is where the
// original decides what a car sounds like. Its shape is worth stating, because it is not the
// obvious one:
//
//   FOUR CONTINUOUS VOICES per vehicle, each gated by one flag the physics tick leaves behind:
//
//     skid    the tyres are sliding      -> `hnd fx skid   <surface>`
//     ground  the car is actually moving -> `hnd fx ground <surface>`
//     engine  the car is not destroyed   -> `hnd fx engine 0`
//     nitro   the boost is running       -> `hnd fx nitro  0`
//
//   Each is restarted when the SURFACE CHANGES, because the surface picks the sample - that is
//   a test the original makes too, and it is why driving from tarmac into water
//   swaps the rolling sound instead of cross-fading it.
//
//   Plus a queue of ONE-SHOTS (impacts, landings) drained in the same pass.
//
// ⚠️ Driven by the TICK, not by the frame. Edges - a landing, an impact, a refused boost - exist
// for exactly one tick, and a frame that happens to span two ticks would swallow one. The
// original ticks its audio too (every 8 timer ticks); ours runs on every physics tick, which is
// finer, not coarser.
//
// ⚠️ Nothing here may be read by the simulation. Whether a `.WAV` is on disk must never change
// what the physics does.
#pragma once

#include <cstdint>
#include <string>

#include "../formats/vehicle_script.h"
#include "mixer.h"
#include "sound_bank.h"

namespace bigred {

class VehicleSim;

class VehicleAudio {
public:
    void configure(const VehicleTuning* tuning, SoundBank* bank, Mixer* mixer);
    // One physics tick. `distance` is how far the listener is from the car in world units, and
    // `pan` is 0..256 left-to-right; a single-player chase camera passes the rope length and 128.
    void on_tick(const VehicleSim& car, int32_t distance, int32_t pan);

    // A race event rather than something the car did to itself: a checkpoint passed, a lap
    // completed. It goes out on the `hnd fx check` table, whose indices the scripts document
    // themselves - 0 checkpoint, 1 lap completed, 2/4 checkpoint missed, 3 wrong way, 5 jingle.
    // Most vehicles declare none of them, and then this is silent, which is the shipped data.
    void on_race_event(int check_index, int32_t distance, int32_t pan);

    // Another car ran into this one: the original's contact routine queues `hit 2` ("same car" -
    // the same model) or `hit 3` ("other car") on the car that was HIT, at full intensity (0x100).
    // At this car's own distance and pan.
    void on_car_contact(bool same_model, int32_t distance, int32_t pan);

    void stop();

    // For the bench: what the last tick DECIDED, which is not the same as what is audible.
    // Three states have to be told apart, because "I hear nothing" has three causes:
    //   want      the simulation flag is up - the car is sliding, the engine is alive...
    //   declared  ...and this vehicle has a `hnd fx` line for that surface. Often it does not,
    //             and that is the shipped data: a JEEP has no rolling sound on the track at all.
    //   playing   ...and a voice is actually running. False with no audio device, which is
    //             exactly how the bench runs.
    struct SlotDebug {
        bool want = false, declared = false, playing = false;
        // How many times this voice has been (re)STARTED over the run. It is the number that
        // tells a sustained voice from a machine-gun: a car that crosses two surfaces should
        // start its rolling voice twice, not three hundred times. Cumulative, and counted even
        // with no audio device, because it is a decision and not an outcome.
        int starts = 0;
    };
    struct Debug {
        int32_t volume = 0;
        SlotDebug skid, ground, engine, nitro;
        int land = 0;
        int one_shots = 0;      // fired on the last tick
        // Tallied for the whole run, not per tick: which one-shots actually got asked for is
        // the only way to tell "the trigger never fires" from "the sample is missing".
        int n_land = 0, n_hit = 0, n_splat = 0, n_notturb = 0, n_explode = 0;
        // The loudest one-shot volume asked for over the run. The user heard impacts drowning
        // everything; this is the number that says whether they still can.
        int32_t peak_one_shot = 0;
        const char* last_event = "";
    };
    const Debug& debug() const { return debug_; }

private:
    // One of the four continuous voices.
    struct Continuous {
        VoiceId voice;
        int sample_index = -1;  // the fx index currently playing, so a surface change restarts it
        bool on = false;
    };

    const FxSound* entry(FxCategory cat, int index) const;
    void drive(Continuous& c, FxCategory cat, int index, bool want, int32_t volume, int32_t pan,
               int32_t rate_override, SlotDebug& dbg);
    void fire(FxCategory cat, int index, int32_t volume, int32_t pan, int32_t intensity,
              const char* label);

    const VehicleTuning* tuning_ = nullptr;
    SoundBank* bank_ = nullptr;
    Mixer* mixer_ = nullptr;

    Continuous skid_, ground_, engine_, nitro_;
    // The latched state the restart rule compares against - the original's per-vehicle fields.
    int32_t engine_rate_ = 0;   // the eased engine pitch - see on_tick
    int last_land_ = -1;
    uint16_t last_near_ground_ = 0;
    Debug debug_;
};

} // namespace bigred
