#include "tourn.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace bigred {
namespace {

// The script conventions of every text file in this game: `;` starts a comment, commas are
// separators like spaces, and a number is decimal unless it starts with `&`, which is hex.
std::vector<std::string> tokens_of(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : line) {
        if (c == ';') break;
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

int number_of(const std::string& token) {
    if (token.empty()) return 0;
    if (token[0] == '&') return static_cast<int>(std::strtol(token.c_str() + 1, nullptr, 16));
    return static_cast<int>(std::strtol(token.c_str(), nullptr, 10));
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const TournTrack* TournCatalog::find(const std::string& level) const {
    const std::string want = upper(level);
    for (const TournTrack& t : tracks)
        if (t.level == want) return &t;
    return nullptr;
}

int difficulty_of(const std::string& english_description) {
    const std::string d = upper(english_description);
    static const char* kWords[3] = {"EASY", "MEDIUM", "HARD"};
    size_t best = std::string::npos;
    int level = -1;
    for (int i = 0; i < 3; ++i) {
        const size_t at = d.find(kWords[i]);
        if (at != std::string::npos && at < best) {
            best = at;
            level = i;
        }
    }
    return level;
}

TournCatalog parse_tourn_catalog(const std::string& tourn_txt) {
    TournCatalog out;
    std::istringstream in(tourn_txt);
    std::string line;
    // ⚠️ `use` appears in THREE sections - `network`, `single` and each `tourn N` - with a
    // different order in each. Only the `single` one is taken here: it is the order a lone
    // player is offered the circuits in, which is what a menu wants. The rest is the
    // championship's business, not this parser's.
    bool in_single = false;
    int in_round = -1;   // index into out.rounds, -1 outside a `tourn` section

    while (std::getline(in, line)) {
        const std::vector<std::string> t = tokens_of(line);
        if (t.empty()) continue;
        const std::string key = upper(t[0]);

        if (key == "SINGLE") { in_single = true; in_round = -1; continue; }
        if (key == "ENDSINGLE") { in_single = false; continue; }
        if (key == "NETWORK") { in_single = false; in_round = -1; continue; }
        if (key == "TOURN") {
            in_single = false;
            TournRound r;
            r.number = t.size() > 1 ? number_of(t[1]) : static_cast<int>(out.rounds.size());
            out.rounds.push_back(std::move(r));
            in_round = static_cast<int>(out.rounds.size()) - 1;
            continue;
        }
        if (key == "ENDTOURN" || key == "ENDALLTOURN") { in_round = -1; continue; }

        if (key == "USE" && in_single && t.size() > 1) {
            out.single_order.push_back(upper(t[1]));
            continue;
        }
        if (key == "USE" && in_round >= 0 && t.size() > 1) {
            out.rounds[static_cast<size_t>(in_round)].levels.push_back(upper(t[1]));
            continue;
        }
        if (key == "NEED" && in_round >= 0 && t.size() > 1) {
            out.rounds[static_cast<size_t>(in_round)].need = number_of(t[1]);
            continue;
        }
        // track <LEVEL> <globetext> <viewtext> <name>, <intro>, <music>, <x>,<y>,<z>
        //                 t[1]       t[2]        t[3]     t[4]     t[5]     t[6] t[7] t[8] t[9]
        // ⚠️ THE FIRST NUMBER IS NOT A CD TRACK: the executable reads the three in file order
        // into three separate tables, and the first of them is
        // the index of the circuit's hand-written NAME sprite - see tourn.h.
        if (key == "TRACK" && t.size() >= 7) {
            TournTrack track;
            track.level = upper(t[1]);
            track.globe_text = upper(t[2]);
            track.view_text = upper(t[3]);
            track.name_index = number_of(t[4]);
            track.intro_track = number_of(t[5]);
            track.music_track = number_of(t[6]);
            // `&7600,&2800,0` is a position on the globe; `-1,-1,<n>` is a planet of its own.
            // A line that stops short of them simply has neither, and the globe then puts the
            // circuit nowhere rather than at 0,0 - which is the north pole and would be a lie.
            if (t.size() >= 10) {
                const int x = number_of(t[7]), y = number_of(t[8]), z = number_of(t[9]);
                if (x < 0 || y < 0) {
                    track.planet = z;
                } else {
                    track.globe_x = x;
                    track.globe_y = y;
                }
            }
            out.tracks.push_back(std::move(track));
        }
    }
    return out;
}

} // namespace bigred
