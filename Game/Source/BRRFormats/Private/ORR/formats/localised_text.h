// `TOURN.<LANG>` and `START.<LANG>` - the interface's own strings, in the player's language.
//
//
// A `.SCN` widget and a `TOURN.TXT` circuit both name a KEY, never a sentence: `stext 4,
// GLB_HD_WINX` and `track DIRT1 GLB_PLC31 VIE_DT1 ...`. The text lives here, one table per
// language, both inside `TOURN.WAP`.
//
// ⚠️ THE FILE IS WRITTEN FOR TRANSLATORS AND CARRIES COMMANDS, not just text: `@X` positions,
// `@M` inserts another string, `@C` picks a colour ramp, `@F` a font, and `\` is a line break.
// This reader STRIPS them and keeps the words - which is what a list of circuits needs. The day
// a screen needs the layout back, the commands are still in the file and the spec describes
// them; nothing here is lossy on disk.
//
// ⚠️ ACCENTED BYTES ARE PC EXTENDED ASCII (code page 437/850), not UTF-8 - the authors say so
// in the file. They are passed through untouched: the game's own alphabet has its accented
// capitals at its own indices, so a French table will show holes until that mapping is done.
// English, which is the shipped default, has no accent at all.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bigred {

class TextTable {
public:
    // Loads `TOURN.<lang>` and `START.<lang>` from `TOURN.WAP`. False when the archive or the
    // language is not there; every caller must cope, since a missing table costs labels, not
    // the game.
    bool load(const std::string& game_dir, const std::string& lang);

    bool ready() const { return !entries_.empty(); }
    size_t size() const { return entries_.size(); }

    // The text for a key, or `fallback` when the table does not have it. Every line of the
    // entry, `@` commands dropped, joined by a space.
    std::string text(const std::string& key, const std::string& fallback = {}) const;

    // The same, but ONE STRING PER LINE of the entry. The file's line breaks are meaningful -
    // `INF_JEEP` is the vehicle's name on its first line and the kind of machine it is on the
    // second, at their own `@X` positions - and text() joins them into one run-on sentence.
    std::vector<std::string> text_lines(const std::string& key) const;

    // An entry's `@M` REFERENCES, line by line: `@MHND_SPEED @MHND_AV` comes back as the two
    // keys {"HND_SPEED", "HND_AV"}.
    //
    // ⚠️ THIS IS HOW A VEHICLE'S SIX STATISTICS ARE STORED, and they are not computed from
    // anything: `VHC_JEEP` is six lines of two macros, a label and a qualitative word, hand
    // written by the authors ("Top Speed: Average", "Nitro: Canister"). The file says so to its
    // translators in as many words - "there's nothing to translate here".
    //
    // An entry that is nothing but a single reference is an ALIAS and is followed:
    // `VHC_CABMINI` is `@MVHC_MINI`, the convertible Mini driving exactly like the Mini.
    std::vector<std::vector<std::string>> macro_lines(const std::string& key) const;

    // The entry cut where ITS AUTHORS broke it: at every `\`, the original's carriage return,
    // and not at the file's own line ends. An empty string is a `\\` - a blank line, which is
    // how a fact sheet separates its fields ("Name:\Derek...\\Date of Birth:\..."). For text
    // laid out as the authors laid it out: a driver's biography (CHAR1..CHAR6).
    std::vector<std::string> broken_lines(const std::string& key) const;

    // THE ENTRY AS THE ORIGINAL DRAWS IT: every run of text with the place, colour and font
    // its commands give it. `@X<x>,<y>` starts
    // a run at that point of the 320x200 screen, `\` starts the next line under the last `@X`,
    // `@C<n>` picks palette ramp n (the lettering is drawn in shade 2 of it), `@F<n>` a font,
    // and `@M<KEY>` draws another entry right there. The file's own line ends are NOT breaks:
    // the original skips them. A run the entry never placed keeps x = y = -1.
    struct Run {
        int x = -1, y = -1;
        int ramp = -1;     // the `@C` in force, -1 = none said
        int font = -1;     // the `@F` in force, -1 = none said
        std::string text;
    };
    std::vector<Run> layout(const std::string& key) const;

private:
    const std::vector<std::string>* find(const std::string& key) const;
    // One entry turned into display lines, with `@M` references followed. See the .cpp.
    void expand(const std::string& key, int depth, std::vector<std::string>& out) const;
    void lay_out(const std::string& key, int depth, std::vector<Run>& out, int& memo_x) const;

    // The entry's lines AS WRITTEN, commands and all: the commands are what `macro_lines`
    // reads, and stripping them at load would throw away the statistics entirely.
    std::unordered_map<std::string, std::vector<std::string>> entries_;
};

// The language the player's own `BIGRED.CFG` declares (`lang ENG`). "ENG" when the file is not
// there or says nothing - the game ships that way too.
std::string config_language(const std::string& game_dir);
// `-LANG=FRE` on the command line: that language instead of the file's, for this run - the way to
// look at a translation without editing the original install's BIGRED.CFG. Empty = the file.
void set_language_override(const std::string& lang);

// --- the characters ---
//
// The tables are written in the PC's code page 850. TextTable::load converts them to UTF-8, the
// encoding everything else in the program speaks (RmlUi, SDL's text input, the profile), so an
// accent typed by the player and one written by the translators reach the fonts the same way.
// Two bytes are the authors' own symbols rather than letters: 0x9D is the SMILEY of the one/two
// player choices ("Chris has put some ... symbols in the file, which (I think) are the smiley
// faces", TOURN.ENG) and 0xAA the name-entry POINTER; they become U+263A and U+00AC.
std::string cp850_to_utf8(const std::string& bytes);

// The code point at `s[i]`, `i` moved past it. A byte that does not start valid UTF-8 is taken
// as Latin-1 on its own, so a stray byte shows as something rather than swallowing the rest.
char32_t next_code_point(const char* s, size_t n, size_t& i);

// Which glyph of an interface alphabet draws code point `cp`, or -1 for none. Both alphabets
// (SML_ALPH.SPR and the five WIBS) put ' '..'Z' at 0..58 and have NO lower case - it folds to
// the capitals, as the original's table does. Then 59 (c), 60 the pointer, 61 the smiley and the
// accented capitals at 62..78. A letter with an accent they do not draw (the Spanish table's
// a/o/u acute, n tilde) falls back to its bare letter rather than vanishing.
// ⚠️ `wibbly`: the two alphabets disagree on 62 and 63 - SML_ALPH draws E-acute then E-grave,
// the WIBS the other way round. Each is read as it is drawn.
int interface_glyph(char32_t cp, bool wibbly);

} // namespace bigred
