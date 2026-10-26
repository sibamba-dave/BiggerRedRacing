// `TOURN.TXT` (inside `TOURN.WAP`) - the catalogue of circuits, and the championship.
//
//
// It is the file that says WHICH circuits exist, in which order they are offered, and - the
// reason it was opened - **which CD track each one plays**. The authors annotated the columns
// themselves at the top of the file:
//
//     ; wapname, globetext, viewtext
//     ; sems on marker shape
//     ; cd intro track
//     ; cd music track
//     ; xyz coords, or -1,-1,planet
//
//     track    DIRT1      GLB_PLC31 VIE_DT1   13,  21, 3, &7600,&2800,0
//
// ⚠️ TWO TRACK NUMBERS PER CIRCUIT, and they do different things: the **intro** plays once when
// the race starts, then the **music** takes over and loops for as long as the race lasts. That
// is what the original does (its race-music call takes both; its CD handler restarts the second
// one whenever a track ends), and it is why a circuit sounds like itself.
//
// ⚠️ THE FIRST NUMBER IS NOT A MARKER SHAPE. The authors' own header calls it "sems on marker
// shape", and this file used to take them at their word; it is the index of the circuit's
// HAND-WRITTEN NAME, in the sprite sheet `NAMES.TM` + `NAMES2.TM` of `GLOBESCN.SCN`. The proof
// is complete and lies in the data alone: the 24 circuits use exactly 0..22 with 3 appearing twice -
// and the two circuits that share 3 are WATER1 and CITY2, the only two whose country is the
// same (U.S.A.). Laid out column-major in 128x32 cells, all 24 land on their own country's
// name.
#pragma once

#include <string>
#include <vector>

namespace bigred {

// The front end's own track, and it is NOT in any file: `2` is a constant in the executable,
// passed to the player from three different places (two in the front end, and the start-up
// retry loop). On a mixed-mode CD track 1 is the DATA track, so 2 is the first
// piece of music on the disc.
constexpr int kFrontEndMusicTrack = 2;

struct TournTrack {
    std::string level;      // the `.LEV` name: "DIRT1"
    // ⚠️ WHAT A PLAYER IS SHOWN IS THE PLACE, NOT THE FILE. `globe_text` is a key into the
    // localised tables (formats/localised_text.h) and resolves to a country - `GLB_PLC31` is
    // JAPAN, `GLB_P_R1` is Kenya. The original never displays "DIRT1" anywhere.
    std::string globe_text; // the name on the globe screen
    std::string view_text;  // the name on the circuit's presentation screen
    int intro_track = 0;    // CD track played once at the start, 0 = none declared
    int music_track = 0;    // CD track looped for the rest of the race
    // Which hand-written name this circuit shows on the globe - see the ⚠️ above.
    int name_index = -1;

    // --- where the circuit sits on the globe -------------------------------------------
    // ⚠️ BOTH ARE 16-BIT BINARY ANGLES, 0x10000 to the turn, and they are NOT longitude and
    // latitude in the usual sense:
    //
    //   `globe_x` IS the longitude on the globe's own mesh, straight: a turn of `0x10000` is
    //             a turn of the sphere, and `Globe::yaw_for()` is just `globe_x / 65536`. No
    //             calibration is involved - the mesh carries the offset, as a quarter-turn
    //             phase on its own longitude (ui/globe.cpp, `kLonPhase`);
    //   `globe_y` is the COLATITUDE, 0 at the north pole. SNOW1 is ARCTIC and its line reads
    //             `0,0,0`: the pole, exactly, which is what fixes the convention.
    //
    // A `planet` other than 0 means the line read `-1,-1,<n>` and there is no position at all:
    // the whole body IS the destination. 1 = Mars (MOON1), 2 = Venus (MOON2), 3 = the Moon
    // (MOON3) - which is what their own country names say.
    int globe_x = 0, globe_y = 0;
    int planet = 0;
};

// One round of the championship: a `tourn N` section. The original's loader reads it - `use` adds a
// circuit, `need` is how many of them have to be WON to go on. ⚠️ Its `points` line is NOT
// read by the executable (the loader's switch has no case for it): a championship race scores
// a win or nothing, which is all `points 1,0,0,0,0,0` says anyway.
struct TournRound {
    int number = 0;                      // N, as written
    std::vector<std::string> levels;     // in the file's order
    int need = 1;
};

struct TournCatalog {
    std::vector<TournTrack> tracks;          // in the file's own order
    std::vector<std::string> single_order;   // the `single` section's `use` list, in order
    // The championship, `tourn 0` .. `tourn 6`, in file order. ⚠️ The shipped `tourn 2` has no
    // `endtourn`: the next `tourn` closes it, as it does in the game's own loader.
    std::vector<TournRound> rounds;

    const TournTrack* find(const std::string& level) const;
    bool empty() const { return tracks.empty(); }
};

// A circuit's DIFFICULTY, 0 easy / 1 medium / 2 hard, -1 unknown. It is not a field anywhere:
// it is the first word of the circuit's description, the `VIE_*` string of its `track` line
// ("Easy - A rocky Rig and Truck circuit", "Medium - ...", "Hard - ..."), or the first of the
// three words met in it ("An easy dirt track" - DIRT2). Read in the ENGLISH table whatever the
// player's language, so the rule does not depend on a translation.
int difficulty_of(const std::string& english_description);

// Parses the text of `TOURN.TXT`. Returns what it understood; a file that is not there or not
// readable gives an empty catalogue, and every caller must cope - the game is playable without
// it, just without its music mapping.
TournCatalog parse_tourn_catalog(const std::string& tourn_txt);

} // namespace bigred
