#include "shading.h"

namespace bigred {

const uint8_t* shade_row(const uint8_t* cg, size_t cg_size, int level) {
    if (!cg || cg_size < kShadeTableSize) return nullptr;
    if (level < 0) level = 0;
    if (level > kShadeLevels - 1) level = kShadeLevels - 1;
    return cg + static_cast<size_t>(kShadeHeaderRows + level) * kShadeRowSize;
}

std::vector<RGB> brighten_palette(const std::vector<RGB>& palette, const uint8_t* cg, size_t cg_size) {
    const uint8_t* row = shade_row(cg, cg_size, kShadeLevels - 1);
    if (!row) return palette;
    std::vector<RGB> out(palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        // An index outside the loaded palette can't be resolved; keep the source colour rather
        // than reading out of bounds. (Doesn't happen with a real 256-entry MAP.COL.)
        size_t dst = row[i & 0xff];
        out[i] = (dst < palette.size()) ? palette[dst] : palette[i];
    }
    return out;
}

float shade_radius_world(int view_distance_cells) {
    return static_cast<float>((view_distance_cells - 2) * 0x200);
}

} // namespace bigred
