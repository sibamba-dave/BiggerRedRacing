#include "localised_text.h"

#include <cctype>
#include <sstream>
#include <vector>

#include "game_files.h"

namespace bigred {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// A key line is nothing but `[A-Z0-9_]`, which is what tells one apart from a line of text.
bool is_key(const std::string& s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (!(std::isupper(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

// Drops the `@` commands and turns `\` into a space. See the header: the layout is not lost,
// it is simply not what a caller of this reader is after.
std::string strip_commands(const std::string& line) {
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '@') {
            // A command runs to the next space: `@X30,40`, `@MHND_ACCEL`, `@C3`, `@W`.
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            continue;
        }
        if (line[i] == '\\') { out.push_back(' '); continue; }
        out.push_back(line[i]);
    }
    return trim(out);
}

// ⚠️ THE LINES ARE KEPT AS WRITTEN, commands included, and stripped on the way out instead.
// This reader used to strip at load and keep only the words, which silently lost two things:
// the line breaks (`INF_JEEP`'s name and its vehicle type became one run-on string) and every
// entry that is ONLY commands - which is all twenty vehicle statistic blocks, since a line like
// `@MHND_SPEED @MHND_AV` has no words of its own at all.
void parse_into(const std::string& text,
                std::unordered_map<std::string, std::vector<std::string>>& out) {
    std::istringstream in(text);
    std::string raw;
    std::string key;
    std::vector<std::string> body;

    const auto flush = [&]() {
        if (!key.empty() && !body.empty()) out.emplace(key, std::move(body));
        key.clear();
        body.clear();
    };

    while (std::getline(in, raw)) {
        // `;` to end of line is a comment, in this file like in every script of this game.
        if (const size_t c = raw.find(';'); c != std::string::npos) raw = raw.substr(0, c);
        const std::string line = trim(raw);
        if (line.empty()) { flush(); continue; }   // a blank line separates entries
        if (line == "END") break;
        if (key.empty() && is_key(line)) { key = line; continue; }
        if (key.empty()) continue;                 // text with no key: not ours to guess at
        body.push_back(line);
    }
    flush();
}

// The `@M` references of one line, in order. A command runs to the next space, so the name is
// everything from `@M` up to it.
std::vector<std::string> macros_of(const std::string& line) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] != '@' || (line[i + 1] != 'M' && line[i + 1] != 'm')) continue;
        size_t j = i + 2;
        while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i + 2) out.push_back(upper(line.substr(i + 2, j - i - 2)));
        i = j;
    }
    return out;
}

} // namespace

bool TextTable::load(const std::string& game_dir, const std::string& lang) {
    entries_.clear();
    ArchivePtr wap_archive;
    try {
        wap_archive = game_archive(game_dir, "TOURN.WAP");
    } catch (const std::exception&) {
        return false;
    }
    const Archive& wap = *wap_archive;
    // `TOURN.<LANG>` holds the interface; `START.<LANG>` the handful of strings loaded before
    // the player has a profile. Both, because a caller should not have to know which is which.
    for (const char* stem : {"TOURN.", "START."}) {
        const auto it = wap.find(stem + upper(lang));
        if (it == wap.end()) continue;
        // UTF-8 from here on - except the Japanese table, whose two-byte characters are not
        // code page 850 at all (and which no interface alphabet of the game can draw anyway).
        const std::string raw(it->second.begin(), it->second.end());
        parse_into(upper(lang) == "JAP" ? raw : cp850_to_utf8(raw), entries_);
    }
    return !entries_.empty();
}

const std::vector<std::string>* TextTable::find(const std::string& key) const {
    const auto it = entries_.find(upper(key));
    return it == entries_.end() ? nullptr : &it->second;
}

// `@M` MEANS "PUT THAT MESSAGE HERE", so a line that is nothing but references is not an empty
// line - it is the other entry's text. `INF_CABMINI` is the single line `@MINF_MINI`, the
// convertible Mini being described exactly like the Mini, and reading it without expanding gave
// a vehicle with no name at all.
//
// The depth is bounded rather than tracked: a table that pointed two entries at each other
// would otherwise recurse for ever. Nothing shipped nests more than once.
void TextTable::expand(const std::string& key, int depth, std::vector<std::string>& out) const {
    if (depth > 4) return;
    const std::vector<std::string>* lines = find(key);
    if (!lines) return;
    for (const std::string& line : *lines) {
        const std::string words = strip_commands(line);
        const std::vector<std::string> refs = macros_of(line);
        if (!words.empty() || refs.empty()) {
            if (!words.empty()) out.push_back(words);
            continue;
        }
        if (refs.size() == 1) {
            expand(refs[0], depth + 1, out);    // the whole entry stands in for this line
            continue;
        }
        // Several references on one line - a label and a value, which is how a vehicle's
        // statistics are written. They belong on ONE line, so they are joined rather than
        // spliced.
        std::string joined;
        for (const std::string& ref : refs) {
            std::vector<std::string> part;
            expand(ref, depth + 1, part);
            for (const std::string& p : part) {
                if (!joined.empty()) joined.push_back(' ');
                joined += p;
            }
        }
        if (!joined.empty()) out.push_back(std::move(joined));
    }
}

std::string TextTable::text(const std::string& key, const std::string& fallback) const {
    std::vector<std::string> lines;
    expand(upper(key), 0, lines);
    std::string body;
    for (const std::string& line : lines) {
        if (!body.empty()) body.push_back(' ');
        body += line;
    }
    return body.empty() ? fallback : body;
}

std::vector<std::string> TextTable::text_lines(const std::string& key) const {
    std::vector<std::string> out;
    expand(upper(key), 0, out);
    return out;
}

std::vector<std::vector<std::string>> TextTable::macro_lines(const std::string& key) const {
    std::vector<std::vector<std::string>> out;
    std::string at = upper(key);
    // An alias is followed, not returned: `VHC_CABMINI` is the single line `@MVHC_MINI`. The
    // depth is bounded because a file that pointed two aliases at each other would otherwise
    // spin here for ever - the shipped tables never nest more than once.
    for (int depth = 0; depth < 4; ++depth) {
        const std::vector<std::string>* lines = find(at);
        if (!lines) return out;
        out.clear();
        for (const std::string& line : *lines) {
            std::vector<std::string> refs = macros_of(line);
            if (!refs.empty()) out.push_back(std::move(refs));
        }
        if (out.size() == 1 && out[0].size() == 1 && strip_commands((*lines)[0]).empty()) {
            at = out[0][0];      // a whole entry that is one reference: follow it
            continue;
        }
        return out;
    }
    return out;
}

std::vector<std::string> TextTable::broken_lines(const std::string& key) const {
    std::vector<std::string> out;
    const std::vector<std::string>* lines = find(upper(key));
    if (!lines) return out;
    // The file's own line ends are not breaks: the entry is one run of text, cut at `\`.
    std::string run;
    for (const std::string& line : *lines) {
        std::string kept;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '@') {   // a command: `@X90,40`, `@C2` - layout, not words
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                continue;
            }
            kept.push_back(line[i]);
        }
        if (!run.empty() && run.back() != '\\') run.push_back(' ');
        run += trim(kept);
    }
    std::string piece;
    for (const char ch : run) {
        if (ch == '\\') {
            out.push_back(trim(piece));
            piece.clear();
        } else {
            piece.push_back(ch);
        }
    }
    if (!trim(piece).empty()) out.push_back(trim(piece));
    // A trailing break leaves empty pieces behind it; they carry nothing.
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// --- the entry as the original lays it out ------------------------------------------------------
//
// A command is `@`, one letter, its numbers, and ONE separator after them: the renderer reads the
// number and steps past the next byte. A letter it does not know - `@W` - costs
// that letter and nothing else. Runs that continue the line they are on (after a colour change,
// an inserted entry) are marked x = -2: they sit right after the run before them.
namespace {
constexpr int kContinue = -2;
// Line height for `\`, in 320x200 lines: the wibbly lettering (fonts 2..6, and the stext
// default 5) steps by 30, as every `@X` column of the menus does (40, 70, 100); SML_ALPH by 10.
int line_height(int font) { return (font < 0 || (font >= 2 && font <= 6)) ? 30 : 10; }
int read_int(const std::string& s, size_t& i) {
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; ++i; }
    int v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
    return neg ? -v : v;
}
} // namespace

void TextTable::lay_out(const std::string& key, int depth, std::vector<Run>& out,
                        int& memo_x) const {
    if (depth > 4) return;
    const std::vector<std::string>* lines = find(key);
    if (!lines) return;
    auto current = [&]() -> Run& {
        if (out.empty()) out.push_back(Run());
        return out.back();
    };
    auto restart = [&](int x, int y) {
        Run r;
        r.x = x;
        r.y = y;
        if (!out.empty()) { r.ramp = out.back().ramp; r.font = out.back().font; }
        // A run that never received a word is replaced rather than left empty.
        if (!out.empty() && out.back().text.empty() && x != kContinue) out.back() = r;
        else out.push_back(r);
    };
    for (const std::string& line : *lines) {
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '\\') {
                const Run& at = current();
                restart(memo_x, (at.y < 0 ? 0 : at.y) + line_height(at.font));
                continue;
            }
            if (c != '@' || i + 1 >= line.size()) {
                current().text.push_back(c);
                continue;
            }
            const char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(line[++i])));
            ++i;
            if (cmd == 'X') {
                const int x = read_int(line, i);
                if (i < line.size()) ++i;             // the ','
                const int y = read_int(line, i);
                memo_x = x;
                restart(x, y);
            } else if (cmd == 'C' || cmd == 'F') {
                const int n = read_int(line, i);
                if (!current().text.empty()) restart(kContinue, current().y);
                if (cmd == 'C') current().ramp = n; else current().font = n;
            } else if (cmd == 'M') {
                size_t j = i;
                while (j < line.size() && static_cast<unsigned char>(line[j]) > 0x20) ++j;
                const std::string ref = upper(line.substr(i, j - i));
                i = j;
                if (!current().text.empty()) restart(kContinue, current().y);
                lay_out(ref, depth + 1, out, memo_x);
                restart(kContinue, current().y);
                if (i < line.size()) ++i;             // the separator after the key
                --i;                                   // the loop's own ++i
                continue;
            }
            // The one separator after a command; `@W` and the unknown letters stop here too.
            if (i >= line.size() || line[i] != ' ') --i;
        }
    }
}

std::vector<TextTable::Run> TextTable::layout(const std::string& key) const {
    std::vector<Run> out;
    int memo_x = 0;
    lay_out(upper(key), 0, out, memo_x);
    // Trailing blanks carry nothing; empty runs neither.
    std::vector<Run> kept;
    for (Run& r : out) {
        while (!r.text.empty() && r.text.back() == ' ') r.text.pop_back();
        if (!r.text.empty()) kept.push_back(std::move(r));
    }
    return kept;
}

namespace {
std::string g_language_override;
} // namespace

void set_language_override(const std::string& lang) { g_language_override = upper(lang); }

std::string config_language(const std::string& game_dir) {
    if (!g_language_override.empty()) return g_language_override;
    const Blob cfg = read_file_utf8(game_path(game_dir, "BIGRED.CFG"));
    if (cfg.empty()) return "ENG";
    std::istringstream in(std::string(cfg.begin(), cfg.end()));
    std::string raw;
    while (std::getline(in, raw)) {
        if (const size_t c = raw.find(';'); c != std::string::npos) raw = raw.substr(0, c);
        std::istringstream line(raw);
        std::string key, value;
        if (!(line >> key >> value)) continue;
        if (upper(key) == "LANG") return upper(value);
    }
    return "ENG";
}

namespace {

// Code page 850, 0x80..0xFF, as Unicode. The two symbols of the authors are overridden below.
constexpr char16_t kCp850[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8,
    0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2,
    0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192, 0x00E1,
    0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x00AE, 0x00AC, 0x00BD,
    0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2,
    0x00C0, 0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510, 0x2514, 0x2534,
    0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560,
    0x2550, 0x256C, 0x00A4, 0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580, 0x00D3, 0x00DF, 0x00D4,
    0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE, 0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD,
    0x00AF, 0x00B4, 0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8, 0x00B0,
    0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0};

constexpr char32_t kSmiley = 0x263A, kPointer = 0x00AC;

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The accented capitals of the interface alphabets, by glyph:
// the letter each glyph draws, upper and lower case. 62/63 are SML_ALPH's order; the WIBS swap them.
struct Accented {
    char32_t upper, lower;
    int glyph;
};
constexpr Accented kAccented[] = {
    {0x00C9, 0x00E9, 62}, {0x00C8, 0x00E8, 63}, {0x00CA, 0x00EA, 64}, {0x00C0, 0x00E0, 65},
    {0x00C2, 0x00E2, 66}, {0x00C4, 0x00E4, 67}, {0x00CC, 0x00EC, 68}, {0x00CE, 0x00EE, 69},
    {0x00CF, 0x00EF, 70}, {0x00C7, 0x00E7, 71}, {0x00D2, 0x00F2, 72}, {0x00D4, 0x00F4, 73},
    {0x00D6, 0x00F6, 74}, {0x00D9, 0x00F9, 75}, {0x00DC, 0x00FC, 76}, {0x00DB, 0x00FB, 77},
    {0x00DF, 0x00DF, 78},
};

// The bare letter under an accent the alphabets do not draw.
char bare_letter(char32_t cp) {
    switch (cp) {
    case 0x00C1: case 0x00E1: case 0x00C3: case 0x00E3: case 0x00C5: case 0x00E5: return 'A';
    case 0x00CB: case 0x00EB: return 'E';
    case 0x00CD: case 0x00ED: return 'I';
    case 0x00D3: case 0x00F3: case 0x00D5: case 0x00F5: case 0x00D8: case 0x00F8: return 'O';
    case 0x00DA: case 0x00FA: return 'U';
    case 0x00D1: case 0x00F1: return 'N';
    case 0x00DD: case 0x00FD: case 0x00FF: return 'Y';
    default: return 0;
    }
}

} // namespace

std::string cp850_to_utf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        const unsigned char b = static_cast<unsigned char>(ch);
        if (b < 0x80) { out.push_back(ch); continue; }
        char32_t cp = kCp850[b - 0x80];
        if (b == 0x9D) cp = kSmiley;
        if (b == 0xAA) cp = kPointer;
        append_utf8(out, cp);
    }
    return out;
}

char32_t next_code_point(const char* s, size_t n, size_t& i) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    const int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (extra == 0 || i + static_cast<size_t>(extra) >= n) {
        ++i;
        return b;
    }
    char32_t cp = b & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((c & 0xC0) != 0x80) {   // not UTF-8 after all: the byte alone, as Latin-1
            ++i;
            return b;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    i += static_cast<size_t>(extra) + 1;
    return cp;
}

int interface_glyph(char32_t cp, bool wibbly) {
    if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 'A';
    if (cp >= ' ' && cp <= 'Z') return static_cast<int>(cp) - ' ';
    if (cp == 0x00A9) return wibbly ? 59 : -1;
    if (cp == kPointer) return 60;
    if (cp == kSmiley) return 61;
    for (const Accented& a : kAccented) {
        if (cp != a.upper && cp != a.lower) continue;
        if (wibbly && (a.glyph == 62 || a.glyph == 63)) return 62 + 63 - a.glyph;
        return a.glyph;
    }
    if (const char bare = bare_letter(cp)) return bare - ' ';
    return -1;
}

} // namespace bigred
