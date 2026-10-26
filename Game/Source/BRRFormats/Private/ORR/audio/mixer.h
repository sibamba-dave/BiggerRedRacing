// A small SDL2 mixer: a fixed set of voices, mixed in a callback.
//
// The original's own channel allocation is deliberately NOT
// reproduced - it is DirectSound buffer housekeeping from 1996 (a queue of 4 pending sounds per
// vehicle, a fixed pool of hardware channels). What IS reproduced is everything above it: which
// sample plays for which event, at which pitch, and its two bounds -
//
//   * at most 8 simultaneous sounds, the original's own cap;
//   * at most 3 engine voices at once, so a full grid does not turn into a drone.
//
// SDL_mixer is not used, and not for lack of it: a hand-written mixer is fifty lines, it keeps
// the dependency list at one, and - the real reason - it lets a voice change pitch continuously,
// which is the whole of the engine sound.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sound_bank.h"

namespace bigred {

class MusicPlayer;

// A handle to a playing voice. Stays valid after the voice stops; the generation counter makes a
// stale handle a no-op rather than a wrong-sound bug.
struct VoiceId {
    int slot = -1;
    uint32_t generation = 0;
    bool valid() const { return slot >= 0; }
};

// What a sound is playing with. Volume and rate can be changed while it runs, which is what an
// engine note and a distance fade need.
struct VoiceParams {
    int32_t rate = 11025;   // playback rate in Hz
    int32_t volume = 256;   // 0..256
    int32_t pan = 128;      // 0 = full left, 128 = centre, 256 = full right
    // A spoken line (the commentary, READYGO). ⚠️ AN ASSUMED DEPARTURE: it is not held to
    // kVolumeCap, and while one plays every other voice is
    // ducked to kDuckGain - the user could not hear the commentator over the engines. Fixed at
    // play(); update() does not change it.
    bool speech = false;
};

class Mixer {
public:
    ~Mixer();
    // Opens the device. Returns false if audio is unavailable, and the whole class then degrades
    // to a no-op - a machine with no sound card must still be able to play the game.
    bool open();
    void close();
    bool ready() const { return device_ != 0; }

    // Starts `sample` on a free voice. `loop_start`/`loop_end` are in samples of the source, with
    // -1 meaning "to the end"; a one-shot passes loop = false. Returns an invalid id when every
    // voice is busy - which is the original's behaviour too, not an error.
    // `loop` also means "this is one of the four continuous voices": when every slot is busy a
    // looping request STEALS the oldest one-shot rather than being dropped. The original does
    // not need that - it rebuilds its whole request list every audio update, so the continuous
    // sounds are re-added first every time - but our voices are persistent, so without it a
    // burst of impacts can lock the engine out until the next surface change.
    VoiceId play(const Sample& sample, const VoiceParams& p, bool loop, int32_t loop_start,
                 int32_t loop_end, bool is_engine);
    void update(VoiceId id, const VoiceParams& p);
    void stop(VoiceId id);
    bool playing(VoiceId id) const;
    void stop_all();

    // 0..256, applied on top of every voice. ⚠️ NOT to the music, which carries its own
    // volume: "quieter sound effects" and "quieter music" are two settings, not one.
    void set_master(int32_t v) { master_ = v < 0 ? 0 : (v > 256 ? 256 : v); }
    int32_t master() const { return master_; }
    int active() const;

    // --- the music ------------------------------------------------------------------------
    // The stream is mixed into THIS device's callback rather than opening a second one: one
    // device means one clock, one latency, and - the reason it matters here - this device's
    // own lock is what serialises the decoder thread against the callback. See music.h.
    void set_music(MusicPlayer* music);
    // For the music player: what rate to feed it, and which lock to take. Both are meaningless
    // until open() has succeeded.
    int out_rate() const { return out_rate_; }
    uint32_t device() const { return device_; }

    static constexpr int kVoices = 8;      // the original's own cap
    static constexpr int kEngineVoices = 3;

private:
    struct Voice {
        const int16_t* pcm = nullptr;
        size_t length = 0;
        uint64_t pos_fx = 0;      // 32.32 position in source samples
        uint64_t step_fx = 0;     // source samples per output sample
        int32_t volume = 256;
        int32_t pan = 128;
        bool loop = false;
        size_t loop_start = 0, loop_end = 0;
        bool engine = false;
        bool speech = false;
        bool active = false;
        uint32_t generation = 0;
    };

    // SDL's audio callback. Declared without SDL's own calling-convention macro so this
    // header stays free of SDL.h; on every target this project builds for it is the default.
    static void callback(void* userdata, uint8_t* stream, int len);
    void mix(int16_t* out, int frames);
    // Writes rate/volume/pan into a voice. ⚠️ The caller must already hold the device lock.
    void apply(Voice& v, const VoiceParams& p);

    uint32_t device_ = 0;
    int out_rate_ = 44100;
    // ⚠️ The original clamps EVERY sound's volume to 0x7f - half scale - and
    // that headroom is what keeps eight voices from clipping against each other. Reproduced.
    static constexpr int32_t kVolumeCap = 0x7f;
    static constexpr int32_t kSpeechCap = 256;
    // Every other voice while a line is spoken, /256. Reached over ~3 callbacks (~70 ms) and
    // released over ~6, so neither edge clicks.
    static constexpr int32_t kDuckGain = 96;
    static constexpr int32_t kDuckAttack = 64, kDuckRelease = 32;   // per callback
    int32_t duck_ = 256;   // audio thread only
    int32_t master_ = 256;
    MusicPlayer* music_ = nullptr;
    Voice voices_[kVoices];
    uint32_t next_generation_ = 1;
};

} // namespace bigred
