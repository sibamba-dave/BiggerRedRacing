#include "sprite.h"

namespace bigred {
namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

TopmapProjection topmap_projection(const SpriteGlyph& glyph0) {
    TopmapProjection p;
    p.a = glyph0.field3 & 0xFF;
    p.b = glyph0.field4 & 0xFF;
    p.scale = (glyph0.field4 >> 8) & 0xFF;
    return p;
}

void topmap_pixel(const TopmapProjection& p, int32_t world_x, int32_t world_z, int& px, int& py) {
    if (!p.valid()) { px = py = 0; return; }
    // The original's projection: ((x - A*512) << 7) / scale >> 9, plus 0x38 for the dot's corner
    // and 8 for its centre. C++ division truncates toward zero as the original's does; >> on a
    // negative int is an arithmetic shift, as there.
    px = (((world_x - p.a * 512) * 128) / p.scale >> 9) + 0x38 + 8;
    py = (((p.b * 512 - world_z) * 128) / p.scale >> 9) + 0x38 + 8;
}

std::vector<SpriteGlyph> decode_spr(const uint8_t* data, size_t size) {
    std::vector<SpriteGlyph> out;
    if (!data || size < 2) return out;
    const int count = rd16(data);
    if (count <= 0) return out;
    const size_t table_end = 2 + static_cast<size_t>(count) * 4;
    if (size < table_end) return out;
    // The table's first entry is always exactly the end of the table itself - a useful check
    // that this really is a `.SPR` and not something that happens to start with a plausible
    // count.
    if (rd32(data + 2) != table_end) return out;

    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const uint32_t off = rd32(data + 2 + static_cast<size_t>(i) * 4);
        SpriteGlyph g;
        if (off + 8 > size) { out.push_back(g); continue; }
        g.width = rd16(data + off);
        g.height = rd16(data + off + 2);
        g.field3 = rd16(data + off + 4);
        g.field4 = rd16(data + off + 6);
        const size_t n = static_cast<size_t>(g.width) * g.height;
        if (g.width <= 0 || g.height <= 0 || off + 8 + n > size) {
            g.width = g.height = 0;
            out.push_back(g);
            continue;
        }
        g.pixels.assign(data + off + 8, data + off + 8 + n);
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<uint8_t> sprite_glyph_rgba(const SpriteGlyph& glyph,
                                      const std::vector<RGB>& palette) {
    if (!glyph.valid() || palette.empty()) return {};
    std::vector<uint8_t> out(static_cast<size_t>(glyph.width) * glyph.height * 4, 0);
    for (size_t i = 0; i < glyph.pixels.size(); ++i) {
        const uint8_t index = glyph.pixels[i];
        if (index == 0) continue;   // transparent, everywhere in this game
        const RGB& c = palette[index < palette.size() ? index : 0];
        out[i * 4 + 0] = c.r;
        out[i * 4 + 1] = c.g;
        out[i * 4 + 2] = c.b;
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<SpriteGlyph> decode_fnt(const uint8_t* data, size_t size) {
    std::vector<SpriteGlyph> out;
    // 96 widths then 96 slots of 8 bytes: 864 bytes, which is exactly what MICRO.FNT is.
    if (!data || size < static_cast<size_t>(kFntGlyphCount) * 9) return out;
    out.resize(static_cast<size_t>(kFntGlyphCount));
    for (int i = 0; i < kFntGlyphCount; ++i) {
        const int w = data[i];
        const uint8_t* slot = data + kFntGlyphCount + static_cast<size_t>(i) * 8;
        SpriteGlyph& g = out[static_cast<size_t>(i)];
        g.width = (w > 0 && w <= 5) ? w : (w > 5 ? 5 : 0);
        g.field3 = w;  // the DECLARED width, which is the advance even when the glyph is blank
        g.height = kFntRows;
        if (g.width <= 0) { g.height = 0; continue; }
        g.pixels.assign(static_cast<size_t>(g.width) * kFntRows, 0);
        for (int row = 0; row < kFntRows; ++row) {
            const uint8_t byte = slot[row];
            for (int col = 0; col < g.width; ++col) {
                // ⚠️ The top five bits, most significant first. Reading all eight adds three
                // blank columns and pulls every glyph out of shape - the trap the spec names.
                if ((byte >> (4 - col)) & 1)
                    g.pixels[static_cast<size_t>(row) * g.width + col] = 255;
            }
        }
    }
    return out;
}

} // namespace bigred
