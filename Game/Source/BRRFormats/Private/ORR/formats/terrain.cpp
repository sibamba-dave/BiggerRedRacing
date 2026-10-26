#include "terrain.h"

#include <cctype>
#include <sstream>

namespace bigred {

std::vector<TerrainVertex> build_terrain_mesh(const uint8_t* mph_data, const uint8_t* mpc_data,
                                               const uint8_t* cht_data, size_t cht_size,
                                               int atlas_count,
                                               int width, int height,
                                               const std::vector<RGB>& palette,
                                               const std::vector<RGB>& palette_bright,
                                               float world_to_render_scale,
                                               float height_scale) {
    std::vector<TerrainVertex> out;
    out.reserve(static_cast<size_t>(width - 1) * (height - 1) * 6);

    bool have_cht = cht_data != nullptr && cht_size >= static_cast<size_t>(kMapChtTextureTableOffset) + 256;

    // Per-quad: which atlas/tile, per the terrain texture-tile selection
    // formula - sampled once at the quad's origin texel (matches the original's own
    // single-query-point-per-quad approach).
    auto tile_for_quad = [&](int gx, int gz, int& atlas_out, int& tile_out) -> bool {
        if (!have_cht) return false;
        uint8_t mpc_byte = mpc_data[static_cast<size_t>(gz) * width + gx];
        uint8_t tile_index = cht_data[kMapChtTextureTableOffset + mpc_byte];
        if (tile_index >= kAtlasTileNoTextureThreshold) return false;
        int atlas = tile_index / kAtlasTilesPerAtlas;
        if (atlas >= atlas_count) return false;
        atlas_out = atlas;
        tile_out = tile_index % kAtlasTilesPerAtlas;
        return true;
    };

    // Flat-colour fallback: the palette index is NOT the raw MAP.MPC byte, it is that byte
    // remapped through MAP.CHT's per-theme tint tables, and it is CONSTANT over a triangle -
    // CONFIRMED in the original's two quad emitters, which write `table0[mpc]` into the first of the
    // quad's two queued records and `table1[mpc]` into the second.
    // Sampling MAP.COL[mpc] at each of the four corners instead produced a smooth four-way
    // gradient plate that exists nowhere in the original.
    auto flat_index = [&](int gx, int gz, int table_offset) -> size_t {
        uint8_t mpc_byte = mpc_data[static_cast<size_t>(gz) * width + gx];
        return have_cht ? cht_data[table_offset + mpc_byte] : mpc_byte;
    };

    // Resolved from the palette INDEX, not from a colour: the vertex carries both ends of that
    // index's shading ramp so the shader can mix between them by distance - see shading.h.
    auto colour_pair = [&](size_t index, const std::vector<RGB>& pal) -> RGB {
        if (index < pal.size()) return pal[index];
        return RGB{255, 255, 255};
    };

    auto make_vertex = [&](int gx, int gz, float u, float v, int atlas, bool textured,
                            size_t flat_idx) -> TerrainVertex {
        const RGB flat = colour_pair(flat_idx, palette);
        const RGB flat_lit = colour_pair(flat_idx, palette_bright);
        uint8_t h = mph_data[static_cast<size_t>(gz) * width + gx];
        TerrainVertex vert{};
        // World X is mirrored relative to raw MAP.MPH/MPC column order (session 22) - confirmed
        // user-side by comparing the first turn out of the starting grid against real gameplay
        // (goes left in the real game, was rendering as a right turn here before this fix). Data
        // sampling still uses the original, unmirrored `gx` - only the resulting world position
        // is reflected across the map's width, same convention used for scenery in main.cpp's
        // `place_scenery`/`build_marker_mesh`.
        vert.x = static_cast<float>((width - 1 - gx) * kTerrainTexelWorldSize) * world_to_render_scale;
        vert.z = static_cast<float>(gz * kTerrainTexelWorldSize) * world_to_render_scale;
        vert.y = static_cast<float>(h) * height_scale * world_to_render_scale;
        vert.r = flat.r / 255.0f;
        vert.g = flat.g / 255.0f;
        vert.b = flat.b / 255.0f;
        vert.br = flat_lit.r / 255.0f;
        vert.bg = flat_lit.g / 255.0f;
        vert.bb = flat_lit.b / 255.0f;
        vert.u = u;
        vert.v = v;
        vert.layer = static_cast<float>(atlas);
        vert.has_texture = textured ? 1.0f : 0.0f;
        return vert;
    };

    // UV span of one tile within its atlas, with a small inward inset (matches the original's
    // own texel-bleeding padding) so bilinear filtering never samples
    // a neighbouring tile.
    constexpr float kInset = 0.5f / 32.0f; // half a texel, in tile-local [0,1] units

    for (int gz = 0; gz < height - 1; ++gz) {
        for (int gx = 0; gx < width - 1; ++gx) {
            int atlas = 0, tile = 0;
            bool textured = tile_for_quad(gx, gz, atlas, tile);

            // THE HOLE RULE. MAP.MPC >= 240 on an untextured quad means "draw nothing at all",
            // which is how the original opens the tunnel mouths, clears the ground under the
            // starting grid and hollows out the CITY block underpasses. CONFIRMED in the original:
            // its untextured path is taken for a tile index >= 192, and the whole of the routine
            // that emits that quad - like that of its textured twin - only runs
            // while MAP.MPC[texel] < 0xf0. Nothing reaches the draw queue above the threshold.
            // Cross-checked against the data: 240 is the ONLY value >= 240 any level's MAP.MPC
            // ever uses, it appears on 14 of the 23 levels, 4 to 118 texels each, and every one
            // of them sits within half a texel of a tunnel mouth, a starting-grid piece or a
            // CITY block. Note the guard lives INSIDE the untextured branch, so the rare texel
            // that is both >= 240 and maps to a real tile (SNOW2, SNOW3) still gets textured -
            // matched here deliberately rather than generalised.
            // Sampled once at the quad's origin texel, same convention as tile_for_quad above.
            if (!textured && mpc_data[static_cast<size_t>(gz) * width + gx] >= kMapMpcHoleThreshold) {
                continue;
            }

            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (textured) {
                int col = tile % kAtlasTileCols;
                int row = tile / kAtlasTileCols;
                // V is inverted relative to the tile-index formula's row convention (U is not) -
                // found empirically (session 22): with V unflipped, asymmetric tiles (riverbank
                // edges, a tunnel-mouth tile) rendered with their top/bottom the wrong way up,
                // user-confirmed fixed by swapping v0/v1 here. Same family of bug as `.3DW`'s
                // inverted-Y convention (threedw.h) - this engine seems to consistently store
                // one vertical axis backwards relative to this renderer's assumptions. Applies
                // uniformly to every textured quad (no per-tile distinction needed/possible from
                // the confirmed tile-selection formula alone).
                u0 = (static_cast<float>(col) + kInset) / kAtlasTileCols;
                u1 = (static_cast<float>(col + 1) - kInset) / kAtlasTileCols;
                v1 = (static_cast<float>(row) + kInset) / kAtlasTileRows;
                v0 = (static_cast<float>(row + 1) - kInset) / kAtlasTileRows;
            }

            // One flat colour per triangle, both read at the quad's origin texel - table0 for
            // the first, table1 for the second, exactly as the original fills its two records.
            const size_t c0 = flat_index(gx, gz, kMapChtTintTable0Offset);
            const size_t c1 = flat_index(gx, gz, kMapChtTintTable1Offset);

            // ⚠️ THE SPLIT IS THE MAIN DIAGONAL n00-n11, NOT THE ANTI-DIAGONAL, and it is the
            // same diagonal the simulation uses (SimTerrain::height_fx). Getting
            // this backwards is invisible on flat ground and wrong by up to several HUNDRED world
            // units on a riverbank or a road edge: the two splits of a twisted cell differ at the
            // cell centre by |(n00+n11) - (n01+n10)| * 8 units, which reaches 4048 on WATER2/3.
            // The car then drives on a surface the renderer does not draw.
            //
            // CONFIRMED in the original's terrain drawing: it
            // walks a 64-stride vertex cache and emits {base+0x40, base, base+1} then
            // {base+1, base+0x41, base+0x40}, so its shared edge joins cache (A,B+1) to (A+1,B).
            // Its two loop counters run OPPOSITE ways over the grid - the column counter counts
            // up, the row counter counts DOWN one per inner step - so that anti-diagonal in cache
            // index space is the MAIN diagonal in grid space. Cross-checked on the tint tables,
            // which only line up under this split: the ground sampler picks table0 when fx < fz, i.e.
            // over the {n00,n01,n11} half, and that is exactly the half the original's first
            // record (the one that gets table0) covers.
            //
            // Triangle 1 = the fx < fz half = {n00, n01, n11}, tint table 0.
            out.push_back(make_vertex(gx, gz, u0, v0, atlas, textured, c0));
            out.push_back(make_vertex(gx, gz + 1, u0, v1, atlas, textured, c0));
            out.push_back(make_vertex(gx + 1, gz + 1, u1, v1, atlas, textured, c0));

            // Triangle 2 = the fx > fz half = {n00, n11, n10}, tint table 1. Same winding sign as
            // triangle 1 in render space (both positive about +Y), as before the fix.
            out.push_back(make_vertex(gx, gz, u0, v0, atlas, textured, c1));
            out.push_back(make_vertex(gx + 1, gz + 1, u1, v1, atlas, textured, c1));
            out.push_back(make_vertex(gx + 1, gz, u1, v0, atlas, textured, c1));
        }
    }
    return out;
}

std::optional<TexesDirective> parse_texes_directive(const std::string& level_txt) {
    std::istringstream in(level_txt);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;
        if (keyword == "texes") {
            TexesDirective d;
            if (ls >> d.base_name >> d.count) return d;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<std::string> texes_atlas_filenames(const TexesDirective& texes) {
    std::vector<std::string> out;
    if (texes.base_name.empty() || texes.count <= 0) return out;

    size_t digits_start = texes.base_name.size();
    while (digits_start > 0 && std::isdigit(static_cast<unsigned char>(texes.base_name[digits_start - 1]))) {
        --digits_start;
    }
    if (digits_start == texes.base_name.size()) return out; // no trailing digits - can't derive a series

    std::string prefix = texes.base_name.substr(0, digits_start);
    std::string digits = texes.base_name.substr(digits_start);
    int start_num = std::stoi(digits);
    size_t width = digits.size();

    for (int i = 0; i < texes.count; ++i) {
        std::ostringstream oss;
        oss << prefix;
        oss.width(static_cast<std::streamsize>(width));
        oss.fill('0');
        oss << (start_num + i);
        out.push_back(oss.str());
    }
    return out;
}

} // namespace bigred
