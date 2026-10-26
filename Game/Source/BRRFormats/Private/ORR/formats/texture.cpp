#include "texture.h"

namespace bigred {

std::vector<uint8_t> decode_tm_rgba(const uint8_t* tm_data, const std::vector<RGB>& palette) {
    std::vector<uint8_t> out(kTmSize * 4);
    for (size_t i = 0; i < kTmSize; ++i) {
        uint8_t idx = tm_data[i];
        RGB c = (idx < palette.size()) ? palette[idx] : RGB{255, 0, 255};
        out[i * 4 + 0] = c.r;
        out[i * 4 + 1] = c.g;
        out[i * 4 + 2] = c.b;
        // Palette index 0 is the transparent/chroma key - CONFIRMED (session 2026-09-12) by
        // counting its use across every .TM in CHOPPER1.LEV: it is EXACTLY 0% of all four
        // terrain ground atlases (TEX11-TEX14, which are opaque full-bleed tiles) yet 17-77% of
        // every decor/sprite texture (START 37%, CLOUDS 53%, GRASS 54%, SIGN 78%) - i.e. it is
        // only ever used where a cut-out background is needed. Before this, those pixels were
        // drawn as palette colour 0 (black), which is why banners and cloud sprites rendered
        // with solid black rectangles around the artwork.
        out[i * 4 + 3] = (idx == 0) ? 0 : 255;
    }
    return out;
}

std::vector<uint8_t> decode_raw_rgba(const uint8_t* raw_data, size_t size) {
    if (!raw_data || size != kRawSize) return {};
    const uint8_t* pix = raw_data + kRawPaletteBytes;
    const size_t count = static_cast<size_t>(kRawWidth) * kRawHeight;
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t idx = pix[i];
        const uint8_t* c = raw_data + static_cast<size_t>(idx) * 3;
        // VGA's 6-bit DAC scaled to 8 bits, the same *4 as load_palette_rgb.
        out[i * 4 + 0] = static_cast<uint8_t>(c[0] << 2);
        out[i * 4 + 1] = static_cast<uint8_t>(c[1] << 2);
        out[i * 4 + 2] = static_cast<uint8_t>(c[2] << 2);
        out[i * 4 + 3] = 255;  // opaque: see the header - a background has nothing behind it
    }
    return out;
}

} // namespace bigred
