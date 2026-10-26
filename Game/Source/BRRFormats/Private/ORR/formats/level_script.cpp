#include "level_script.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace bigred {

int LevelAnim::step_at(uint32_t elapsed_ms) const {
    const int n = step_count();
    if (n <= 0 || period <= 0) return 0;
    return static_cast<int>((elapsed_ms / static_cast<uint32_t>(step_ms())) % static_cast<uint32_t>(n));
}

namespace {

// A level script line is comma-separated with a `;` comment tail, and the numbers are plain
// decimal here (the `&hex` form other keywords use never appears on an `anim` line).
std::vector<long> parse_numbers(const std::string& line_after_keyword) {
    std::vector<long> out;
    std::string cur;
    for (char c : line_after_keyword) {
        if (c == ';') break;
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            cur += c;
        } else if (!cur.empty()) {
            out.push_back(std::stol(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::stol(cur));
    return out;
}

} // namespace

std::vector<LevelAnim> parse_anim_directives(const std::string& level_txt) {
    std::vector<LevelAnim> out;
    std::istringstream in(level_txt);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;
        // `palanim` is a different keyword (palette cycling) - match `anim` exactly.
        if (keyword != "anim") continue;
        std::string rest;
        std::getline(ls, rest);
        const std::vector<long> n = parse_numbers(rest);
        // period, count, mask, then `count` values. The original reads them in exactly this
        // order, so a short line is a malformed one.
        if (n.size() < 3) continue;
        LevelAnim a;
        a.period = static_cast<int>(n[0]);
        const long count = n[1];
        a.mask = static_cast<uint8_t>(n[2]);
        if (a.period <= 0 || count <= 0 || static_cast<size_t>(3 + count) > n.size()) continue;
        a.values.reserve(static_cast<size_t>(count));
        for (long i = 0; i < count; ++i) a.values.push_back(static_cast<uint8_t>(n[3 + i]));
        out.push_back(std::move(a));
    }
    return out;
}

namespace {

// The vehicle-script number syntax also appears in level scripts (`grid &0F401,...`): decimal or
// `&hex`, optionally negated. `parse_numbers` above deliberately does not handle `&`, because an
// `anim` line never uses it.
bool parse_scalar(const std::string& tok, int32_t& out) {
    if (tok.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (tok[i] == '-') { neg = true; ++i; }
    if (i >= tok.size()) return false;
    long long v = 0;
    if (tok[i] == '&') {
        if (++i >= tok.size()) return false;
        v = static_cast<long long>(std::strtoull(tok.c_str() + i, nullptr, 16));
    } else {
        if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return false;
        v = std::strtoll(tok.c_str() + i, nullptr, 10);
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

// Every number on a line, in order, comment tail cut. Commas and whitespace both separate.
std::vector<int32_t> scalars_of(const std::string& line) {
    std::vector<int32_t> out;
    std::string cur;
    const std::string body = line.substr(0, line.find(';'));
    for (char c : body) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            int32_t v;
            if (!cur.empty() && parse_scalar(cur, v)) out.push_back(v);
            cur.clear();
        } else {
            cur += c;
        }
    }
    int32_t v;
    if (!cur.empty() && parse_scalar(cur, v)) out.push_back(v);
    return out;
}

// True when a line carries only numbers - i.e. it is the continuation of the directive above.
bool is_continuation(const std::string& line) {
    const std::string body = line.substr(0, line.find(';'));
    bool any = false;
    for (char c : body) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') continue;
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '&' || c == '-') { any = true; continue; }
        return false;
    }
    return any;
}

// Collects a directive's numbers, following its continuation lines.
std::vector<int32_t> directive_numbers(const std::string& level_txt, const char* keyword) {
    std::istringstream in(level_txt);
    std::string line;
    std::vector<int32_t> out;
    bool collecting = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw == keyword) {
            std::string rest;
            std::getline(ls, rest);
            const std::vector<int32_t> n = scalars_of(rest);
            out.insert(out.end(), n.begin(), n.end());
            collecting = true;
        } else if (collecting && is_continuation(line)) {
            const std::vector<int32_t> n = scalars_of(line);
            out.insert(out.end(), n.begin(), n.end());
        } else if (collecting && !kw.empty()) {
            break;
        }
    }
    return out;
}

} // namespace

std::vector<HorizonObjectDef> parse_horizon_objects(const std::string& level_txt) {
    std::vector<HorizonObjectDef> out;
    std::istringstream in(level_txt);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw, name;
        ls >> kw;
        if (kw != "horizobj" || !(ls >> name)) continue;
        std::string rest;
        std::getline(ls, rest);
        const std::vector<int32_t> n = scalars_of(rest);
        if (n.size() < 3) continue;
        HorizonObjectDef d;
        d.shape = name;
        d.yaw = n[0];
        d.pitch = n[1];
        d.dist = n[2];
        if (n.size() > 5) { d.ax = n[3]; d.ay = n[4]; d.az = n[5]; }
        if (n.size() > 6) d.flags = n[6];
        out.push_back(d);
        if (out.size() >= 20) break;   // the original's table holds 20
    }
    return out;
}

std::vector<uint8_t> parse_land_table(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "land");
    std::vector<uint8_t> out(16, 0);
    for (size_t i = 0; i < out.size() && i < n.size(); ++i)
        out[i] = static_cast<uint8_t>(n[i] & 0xF);
    return out;
}

StartingGrid parse_grid(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "grid");
    StartingGrid g;
    if (n.size() < 3) return g;
    g.present = true;
    g.x = n[0];
    g.z = n[1];
    g.yaw_bam16 = n[2];
    // Then eight triplets, the first two being the two-player layout. A short line leaves the
    // rest zero, which is what the original gets too: its tokenizer yields 0 past the end, so a
    // missing slot is the grid origin itself.
    for (int i = 0; i < 8; ++i) {
        const size_t base = 3 + static_cast<size_t>(i) * 3;
        if (base >= n.size()) break;
        StartingGrid::Slot s;
        s.a = (base + 0 < n.size()) ? n[base + 0] : 0;
        s.b = (base + 1 < n.size()) ? n[base + 1] : 0;
        s.dyaw = (base + 2 < n.size()) ? n[base + 2] : 0;
        if (i < 2) g.two[i] = s; else g.six[i - 2] = s;
    }
    return g;
}

HeliPoint parse_heli(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "heli");
    HeliPoint h;
    if (n.size() < 3 || n[0] == 0) return h;   // the original falls back to the grid on x == 0
    h.present = true;
    h.x = n[0];
    h.z = n[1];
    h.yaw_bam16 = n[2];
    return h;
}

LevelCommentary parse_commentary(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "fx");
    LevelCommentary c;
    if (n.size() > 0) {
        c.jump = n[0] & 0xff;
        c.dead = (n[0] >> 8) & 0xff;
        c.finish = (n[0] >> 16) & 0xff;
    }
    if (n.size() > 1) c.roll = n[1];
    if (n.size() > 2) c.general = n[2];
    return c;
}

int parse_laps(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "laps");
    return n.empty() ? kDefaultLaps : (n[0] > 0 ? n[0] : kDefaultLaps);
}

SunLight parse_light(const std::string& level_txt) {
    SunLight s;
    const std::vector<int32_t> n = directive_numbers(level_txt, "light");
    if (n.size() >= 2) {
        s.elevation = n[0];
        s.azimuth = n[1];
    }
    return s;
}

int parse_countdown_ticks(const std::string& level_txt) {
    const std::vector<int32_t> n = directive_numbers(level_txt, "timing");
    if (n.empty()) return kDefaultCountdownTicks;
    // Case 14's own arithmetic, in its own order: the second number floored at 1, then
    // `(first * 0x46) / 0x2b11` added to it. Every shipped level writes the second as 0.
    const int32_t second = (n.size() > 1 && n[1] != 0) ? n[1] : 1;
    const int32_t ticks =
        static_cast<int32_t>((static_cast<int64_t>(n[0]) * 0x46) / 0x2b11) + second;
    return ticks > 0 ? ticks : kDefaultCountdownTicks;
}

std::vector<FixedCamera> parse_fixed_cameras(const std::string& level_txt) {
    std::istringstream in(level_txt);
    std::string line;
    std::vector<FixedCamera> out;
    while (std::getline(in, line)) {
        const size_t semi = line.find(';');
        std::istringstream ls(line.substr(0, semi));
        std::string kw;
        ls >> kw;
        if (kw != "fixcam") continue;
        std::string rest;
        std::getline(ls, rest);
        const std::vector<int32_t> n = scalars_of(rest);
        if (n.size() < 3) continue;
        FixedCamera c;
        c.x = n[0];
        c.y = n[1];
        c.z = n[2];
        if (semi != std::string::npos) {
            std::string name = line.substr(semi + 1);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t'))
                name.pop_back();
            size_t b = 0;
            while (b < name.size() && (name[b] == ' ' || name[b] == '\t')) ++b;
            c.name = name.substr(b);
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<int> parse_unsafe_gates(const std::string& level_txt) {
    // One gate per line, unlike every other directive here: `unsafe 6` three times over, not
    // `unsafe 6,16,39`. directive_numbers() follows continuation lines, so it would swallow the
    // next `unsafe` line's number as a continuation of this one - which is harmless, since all we
    // want is the set of indices.
    std::istringstream in(level_txt);
    std::string line;
    std::vector<int> out;
    while (std::getline(in, line)) {
        std::istringstream ls(line.substr(0, line.find(';')));
        std::string kw;
        ls >> kw;
        if (kw != "unsafe") continue;
        std::string rest;
        std::getline(ls, rest);
        for (int32_t v : scalars_of(rest)) out.push_back(static_cast<int>(v));
    }
    return out;
}

std::vector<std::string> parse_car_list(const std::string& level_txt) {
    std::istringstream in(level_txt);
    std::string line;
    std::vector<std::string> out;
    while (std::getline(in, line)) {
        std::istringstream ls(line.substr(0, line.find(';')));
        std::string kw, name;
        ls >> kw >> name;
        if (kw == "car" && !name.empty()) out.push_back(name);
    }
    return out;
}

bool is_presentation_vehicle(const std::string& name) {
    return name == "FEHELI" || name == "UFO";
}

NitroReserve parse_nitros(const std::string& level_txt) {
    std::istringstream in(level_txt);
    std::string line;
    NitroReserve out;
    while (std::getline(in, line)) {
        std::string body = line.substr(0, line.find(';'));
        std::istringstream ls(body);
        std::string kw;
        ls >> kw;
        if (kw != "nitros") continue;
        // `nitros 4,800` - one token, comma-separated, and some levels write `nitros  10,2000`.
        std::string rest;
        ls >> rest;
        for (char& c : rest) if (c == ',') c = ' ';
        std::istringstream vs(rest);
        vs >> out.count;
        if (!(vs >> out.full_scale_ticks)) {
            // The comma may have been written with a space after it.
            std::string more;
            std::getline(ls, more);
            for (char& c : more) if (c == ',') c = ' ';
            std::istringstream ms(more);
            ms >> out.full_scale_ticks;
        }
        return out;
    }
    return out;
}

std::vector<SpotEffect> parse_spot_effects(const std::string& env_txt) {
    std::vector<SpotEffect> out;
    std::istringstream in(env_txt);
    std::string line;
    while (std::getline(in, line)) {
        const size_t comment = line.find(';');
        if (comment != std::string::npos) line.erase(comment);
        std::istringstream ls(line);
        std::string keyword;
        if (!(ls >> keyword) || keyword != "spot") continue;
        SpotEffect e;
        if (!(ls >> e.type)) continue;
        if (!(ls >> e.shape)) continue;
        // The three trailing numbers are comma-separated and the authors indent them freely.
        std::string rest;
        std::getline(ls, rest);
        for (char& c : rest) if (c == ',') c = ' ';
        std::istringstream ns(rest);
        if (!(ns >> e.speed)) continue;
        if (!(ns >> e.frame_count)) continue;
        if (!(ns >> e.land)) e.land = -1;
        if (e.speed <= 0) e.speed = 1;
        out.push_back(std::move(e));
    }
    return out;
}

std::string env_name_for_level(const std::string& level_name) {
    std::string s = level_name;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s.empty() ? level_name : s;
}

int anim_slot_of(uint8_t raw_category, uint8_t variant_byte, uint8_t map_dat_offset21) {
    bool animated = false;
    switch (raw_category) {
        case 10: case 20: case 30: case 252:
            animated = true;
            break;
        case 40:
            animated = (variant_byte & 0x40) == 0;
            break;
        default:
            break;
    }
    if (!animated) return -1;
    return map_dat_offset21 % 10;
}

} // namespace bigred
