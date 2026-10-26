// `.SPR` sprite sheets and the `.FNT` bitmap font - everything the interface is drawn with.
//
// Two formats, both small, both fully decoded:
//
//   `.SPR`  a table of offsets followed by one 8-byte header and `w * h` palette indices per
//           glyph. It is NOT only a font container: the in-game set (`GAMEVGA.SPR` /
//           `GAMESVGA.SPR` in INGAME.WAP) holds digits, the ordinals `st`/`nd`/`rd`/`th`, the
//           `WRONG WAY` and `FINAL LAP` banners, `DAMAGE:`, `%` and the six coloured dots the
//           mini-map marks the racers with.
//   `.FNT`  only `MICRO.FNT` uses it: 96 widths then 96 eight-byte slots, of which only the top
//           five BITS and the first five ROWS carry anything. It is an uppercase-only HUD font.
//
// Palette index 0 is transparent, the same convention as every other image in the game.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "threedw.h" // RGB

namespace bigred {

// One decoded `.SPR` entry, still palette-indexed.
struct SpriteGlyph {
    int width = 0, height = 0;
    int field3 = 0;  // constant per file; on GAMEVGA's digits it tracks the width, so it reads
                     // as an ADVANCE - how far to step before the next glyph. Unused elsewhere.
    // Header word +6. Unused by the interface sets; on `TOPMAP.SPR` it is half of the mini-map's
    // projection - see TopmapProjection below.
    int field4 = 0;
    std::vector<uint8_t> pixels;  // width * height palette indices, 0 = transparent
    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height; }
};

// Decodes a whole `.SPR`. Returns an empty vector on anything that does not add up, rather than
// throwing: a missing or truncated interface file must not stop a race.
std::vector<SpriteGlyph> decode_spr(const uint8_t* data, size_t size);

// One decoded glyph as interleaved RGBA8 (`width * height * 4` bytes), through `palette`.
// Index 0 comes out fully transparent, the convention of every image in the game. An invalid
// glyph or an empty palette gives an empty vector - a missing piece of artwork must never be
// fatal to a menu.
//
// ⚠️ WHICH PALETTE IS THE SCREEN'S, not a global one. A `.SPR` carries none: `GLOBESCN.SPR` is
// drawn through `THIRD3.RAW`'s palette because that is the background it sits on, and
// `TRAKSCN.SPR` through the LEVEL's `VIEW.RAW` - which is why TRAKSCN ships no background of
// its own.
std::vector<uint8_t> sprite_glyph_rgba(const SpriteGlyph& glyph, const std::vector<RGB>& palette);

// `TOPMAP.SPR`'s projection, which lives in its glyph header and nowhere else.
//
//   header +4 low byte  A      the terrain cell (X) at the centre of the image
//   header +6 low byte  B      the terrain cell (Z) at the centre of the image
//   header +6 high byte scale  one image pixel is 4 * scale world units (on the 128 glyph)
//
// Raw world coordinates, NOT the renderer's mirrored X; world +Z runs UP the image.
struct TopmapProjection {
    int a = 0, b = 0, scale = 0;
    bool valid() const { return scale > 0; }
};
TopmapProjection topmap_projection(const SpriteGlyph& glyph0);
// Where a racer's marker CENTRE falls, in pixels of the 128x128 glyph. The original draws the
// 16-pixel dot's corner at `... + 0x38`, so its centre is at +64 - the image centre. Integer
// arithmetic exactly as the original's, so a dot lands on the same pixel.
void topmap_pixel(const TopmapProjection& p, int32_t world_x, int32_t world_z, int& px, int& py);

// `MICRO.FNT`, decoded into the same shape so one renderer covers both. Glyph `i` is ASCII
// `32 + i`; indices 64-95 (the lowercase range) are blank in the shipped file and come back as
// empty glyphs with their declared width, which is what makes the font uppercase-only.
//
// The two traps the spec names, both reproduced here: only bits 4..0 of a row byte are pixels,
// and only the first five rows of the eight-byte slot are the glyph.
constexpr int kFntGlyphCount = 96;
constexpr int kFntRows = 5;
std::vector<SpriteGlyph> decode_fnt(const uint8_t* data, size_t size);

// `SML_ALPH.SPR` - the interface's OWN alphabet, and the one the HUD writes with. Plain `.SPR`,
// so `decode_spr` reads it; what matters to a caller is the mapping and the two colours:
//
//   * glyph `i` is ASCII `32 + i`, exactly like MICRO.FNT, from space to `Z` (index 58). The
//     20 entries after it are the French accented capitals, which no HUD string needs;
//   * every glyph is 8x8 and uses exactly TWO palette indices - 16 (white) and 19 (black).
//     The black is an OUTLINE round the white, which is what makes it readable over terrain,
//     and it is why the HUD tints the font instead of drawing it as a mask: a mask would throw
//     the outline away. Tinting keeps it, since any colour times black is still black.
//
// ⚠️ MICRO.FNT is 5 rows tall and carries no outline: blown up to HUD size it is unreadable,
// which the user reported on the first race. It stays as the fallback and nothing more.
constexpr int kSmlAlphGlyphCount = 79;
constexpr int kSmlAlphRows = 8;

// The sprite indices of the in-game set, `GAMEVGA.SPR` and `GAMESVGA.SPR`. Both files carry the
// same 41 entries in the same order at two resolutions - the VGA one and the SVGA one - which is
// what the names say and what a glyph-by-glyph render confirms.
namespace game_spr {
constexpr int kDigit0 = 0;          // 0..9, the large digits
constexpr int kOrdinalSt = 10;      // st, nd, rd, th
constexpr int kDot0 = 14;           // six coloured dots: the mini-map's racer markers
constexpr int kWrongWay = 20;
constexpr int kFinalLap = 21;
constexpr int kDamageIcon = 22;     // three frames of the same red mark
constexpr int kDamageWord = 25;     // "DAMAGE:"
constexpr int kPercent = 26;
constexpr int kSmallDigit0 = 27;    // 0..9 again, smaller - the damage percentage
constexpr int kDot6 = 37;
constexpr int kBar = 39;
constexpr int kMissedCheck = 40;
constexpr int kCount = 41;
} // namespace game_spr

} // namespace bigred
