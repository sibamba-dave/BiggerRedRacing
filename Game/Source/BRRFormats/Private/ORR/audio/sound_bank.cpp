#include "sound_bank.h"

#include "../formats/game_files.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace bigred {
namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

Sample decode_wav(const uint8_t* data, size_t size) {
    Sample out;
    if (size < 44 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return out;

    // Walk the chunks rather than assuming the canonical 44-byte layout. Every file the game
    // ships IS canonical, but a chunk walk costs four lines and cannot be wrong.
    size_t pos = 12;
    uint16_t channels = 1, bits = 8, format = 1;
    uint32_t rate = 11025;
    bool have_fmt = false;
    while (pos + 8 <= size) {
        const uint32_t id = rd32(data + pos);
        const uint32_t len = rd32(data + pos + 4);
        const size_t body = pos + 8;
        if (body + len > size) break;
        if (id == 0x20746d66u) { // "fmt "
            if (len < 16) return out;
            format = rd16(data + body);
            channels = rd16(data + body + 2);
            rate = rd32(data + body + 4);
            bits = rd16(data + body + 14);
            have_fmt = true;
        } else if (id == 0x61746164u) { // "data"
            if (!have_fmt || format != 1 || channels != 1) return out;
            if (bits == 8) {
                // Unsigned 8-bit, silence at 0x80 - the game's own format.
                out.pcm.resize(len);
                for (uint32_t i = 0; i < len; ++i) {
                    out.pcm[i] = static_cast<int16_t>((static_cast<int>(data[body + i]) - 128) * 256);
                }
            } else if (bits == 16) {
                const uint32_t n = len / 2;
                out.pcm.resize(n);
                for (uint32_t i = 0; i < n; ++i)
                    out.pcm[i] = static_cast<int16_t>(rd16(data + body + i * 2));
            } else {
                return out;
            }
            out.rate = static_cast<int>(rate);
            return out;
        }
        pos = body + len + (len & 1); // chunks are word-aligned
    }
    return out;
}

void SoundBank::configure(std::string game_dir, std::string vehicle, std::string level,
                          std::string env) {
    game_dir_ = std::move(game_dir);
    vehicle_ = std::move(vehicle);
    level_ = std::move(level);
    env_ = std::move(env);
    cache_.clear();
    known_bad_.clear();
    misses_ = 0;
}

std::string SoundBank::resolve(const std::string& dir_prefix, const std::string& name) const {
    if (name.empty()) return std::string();
    const std::string file = upper(name) + ".WAV";
    // The prefix names the folder; an empty prefix is the generic pool. Confirmed by a comment
    // in JEEP.TXT itself.
    std::string folder;
    if (dir_prefix == "vhc") folder = vehicle_;
    else if (dir_prefix == "lev") folder = level_;
    else if (dir_prefix == "env") folder = env_;
    else if (dir_prefix == "front") folder = "SAMPLES";
    else folder = "GEN";
    if (folder.empty()) return std::string();
    const std::string path = game_path(game_path(game_dir_, upper(folder)), file);
    if (file_exists_utf8(path)) return path;
    // ⚠️ NO FALLBACK, deliberately. `GEN` is the default for a line with NO prefix - that is what
    // JEEP.TXT's own comment says, "sinon, prendre dans le répertoire gen" - and NOT a second
    // place to look when a prefixed lookup fails. A fallback WAS written here first and then
    // removed: measured across all 20 vehicles it resolved nothing the direct lookup had not
    // already found, and it would have invented a sound where the original is silent.
    //
    // A prefixed miss is normal, and it is the shipped data talking: an `env` sample exists only
    // for the family that uses it, so a BEETLE's `env sheep2` resolves on the CITY circuits and
    // nowhere else. That is correct - there are no sheep on the dirt tracks.
    return std::string();
}

const Sample* SoundBank::get(const std::string& dir_prefix, const std::string& name) {
    const std::string path = resolve(dir_prefix, name);
    if (path.empty()) {
        if (!name.empty() && known_bad_.emplace(dir_prefix + "/" + name, true).second) ++misses_;
        return nullptr;
    }
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second.valid() ? &it->second : nullptr;
    const Blob raw = read_file_utf8(path);
    Sample s = raw.empty() ? Sample() : decode_wav(raw.data(), raw.size());
    auto ins = cache_.emplace(path, std::move(s));
    return ins.first->second.valid() ? &ins.first->second : nullptr;
}

} // namespace bigred
