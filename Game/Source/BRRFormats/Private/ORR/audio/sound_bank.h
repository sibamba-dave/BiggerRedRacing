// Loading the game's sound samples, and finding them on disk.
//
// Two things this has to get right, and neither is the decoding:
//
//   * WHERE a sample lives. A `hnd fx` line carries a directory PREFIX, and the prefix is the
//     lookup rule: `vhc` means the vehicle's own folder, `lev` the circuit's, `env` the theme's,
//     `front` the menu pool, and nothing at all means the generic `GEN` pool. The same stem can
//     exist in two of them - `SHEEP2` is in `CITY/`, `MARSBULB` in `MOON/`.
//   * that a MISSING sample is normal. Several environments have no folder at all (there is no
//     `DIRT/`), so a line that asks for one resolves to nothing. That is the shipped data, not a
//     defect, and it must not be an error.
//
// The files themselves are plain RIFF PCM, 8-bit unsigned mono at 11 025 Hz - the simplest WAV
// there is. Decoded once into 16-bit signed, which is what the mixer wants.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bigred {

struct Sample {
    std::vector<int16_t> pcm;   // mono
    int rate = 11025;           // the file's own rate, before any `hnd fx` override
    bool valid() const { return !pcm.empty(); }
};

// Decodes a RIFF PCM WAV. Handles 8-bit unsigned and 16-bit signed mono, which covers every
// file the game ships; anything else comes back empty rather than throwing.
Sample decode_wav(const uint8_t* data, size_t size);

// Resolves `hnd fx` sample names to files and caches what it loads.
//
// One bank per race. It is a RENDERING-SIDE object: nothing in the simulation may consult it,
// because whether a sound file exists must never change what the physics does.
class SoundBank {
public:
    // `game_dir` is the install root; `vehicle` and `level` name the folders the `vhc` and `lev`
    // prefixes point at, and `env` the theme folder (DIRT1 -> the `env` prefix means `DIRT/`,
    // which happens not to exist - see the header comment).
    void configure(std::string game_dir, std::string vehicle, std::string level, std::string env);

    // The sample for one `hnd fx` entry, loaded on first use and kept. Returns nullptr when the
    // file is not on disk - a normal outcome, see above.
    const Sample* get(const std::string& dir_prefix, const std::string& name);

    // Where a name resolves, or "" if nowhere. Public because the bench reports it.
    std::string resolve(const std::string& dir_prefix, const std::string& name) const;

    size_t loaded() const { return cache_.size(); }
    size_t misses() const { return misses_; }

private:
    std::string game_dir_, vehicle_, level_, env_;
    std::unordered_map<std::string, Sample> cache_;   // by resolved path
    std::unordered_map<std::string, bool> known_bad_; // paths already found missing
    size_t misses_ = 0;
};

} // namespace bigred
