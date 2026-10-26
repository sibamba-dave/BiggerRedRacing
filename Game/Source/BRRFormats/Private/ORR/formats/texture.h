// .TM texture decoding (256x192, 8-bit palette-indexed - research/red-image's TM_SIZE=49152).
#pragma once

#include "threedw.h" // RGB

#include <cstdint>
#include <vector>

namespace bigred {

constexpr int kTmWidth = 256;
constexpr int kTmHeight = 192;
constexpr size_t kTmSize = static_cast<size_t>(kTmWidth) * kTmHeight; // 49152

// Decodes a raw .TM buffer (must be exactly kTmSize bytes) into interleaved RGBA8 pixels
// (kTmWidth*kTmHeight*4 bytes), via the given palette (see threedw.h's load_palette_rgb).
// Palette index 0 is emitted transparent (alpha 0) - see the .cpp for the evidence.
std::vector<uint8_t> decode_tm_rgba(const uint8_t* tm_data, const std::vector<RGB>& palette);

// `.RAW` - the menus' full-screen backgrounds.
//
//   768 bytes   its OWN palette, 256 * RGB at VGA 6-bit (0-63), scaled *4 here
//   64000 bytes 320 * 200 palette indices, top row first
//
// ⚠️ THE PALETTE COMES FIRST, and it is the file's own - not DEFAULT.COL. Every menu screen
// carries a different one, which is why a background drawn through the game palette comes out
// in the wrong colours. Confirmed on all 17 shipped `.RAW`: not one of the 768 leading bytes
// exceeds 63, while the trailing 768 are ordinary image data.
//
// ⚠️ And index 0 is OPAQUE here, unlike a `.TM`: a background has nothing behind it, and the
// original's screens genuinely use colour 0 as a drawn black.
constexpr int kRawWidth = 320;
constexpr int kRawHeight = 200;
constexpr size_t kRawPaletteBytes = 768;
constexpr size_t kRawSize = kRawPaletteBytes + static_cast<size_t>(kRawWidth) * kRawHeight; // 64768

// Returns kRawWidth*kRawHeight*4 RGBA8 bytes, or an empty vector if `size` is not kRawSize.
// A missing or malformed background must not stop the menu from working.
std::vector<uint8_t> decode_raw_rgba(const uint8_t* raw_data, size_t size);

} // namespace bigred
