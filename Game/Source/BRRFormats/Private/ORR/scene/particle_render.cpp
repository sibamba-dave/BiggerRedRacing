#include "particle_render.h"

#include <cstdio>

#include "../formats/texture.h"
#include "../formats/threedw.h"
#include "vehicle_render.h"

namespace bigred {
namespace {

const Blob* find(const std::unordered_map<std::string, Blob>& a, const std::string& name) {
    auto it = a.find(name);
    return it == a.end() ? nullptr : &it->second;
}


} // namespace

ParticleVisuals build_particle_visuals(const std::vector<SpotEffect>& spots,
                                       const std::unordered_map<std::string, Blob>& env,
                                       LevelScene& scene) {
    ParticleVisuals out;
    out.kinds.resize(spots.size());
    if (spots.empty()) return out;

    // One sheet per name, shared: every effect of an environment reads `SPOTFX.TM` or
    // `SPOTFX2.TM`, and the shape NAMES ITS OWN - never its filename. Same rule as the decor's.
    std::unordered_map<std::string, int> sheet_layer;
    auto layer_for = [&](const ThreeDW& m) -> int {
        if (m.embedded_texture_name.empty()) return -1;
        auto it = sheet_layer.find(m.embedded_texture_name);
        if (it != sheet_layer.end()) return it->second;
        const Blob* tm = find(env, m.embedded_texture_name + ".TM");
        // `emissive` makes the atlas's lit copy equal its raw one, so terrain shading cannot
        // darken a puff of dust. That is what these are: light, not lit.
        const int layer = tm ? scene.add_atlas_texture(*tm, /*emissive=*/true) : -1;
        sheet_layer.emplace(m.embedded_texture_name, layer);
        return layer;
    };

    // A shape appears on several `spot` lines (DUST1_FX is declared once per land type), so the
    // bake is cached by name AND frame count - the two lines can ask for different lengths.
    std::unordered_map<std::string, ParticleKind> baked;
    int loaded = 0;
    for (size_t i = 0; i < spots.size(); ++i) {
        const SpotEffect& s = spots[i];
        const std::string key = s.shape + "#" + std::to_string(s.frame_count);
        auto cached = baked.find(key);
        if (cached != baked.end()) {
            out.kinds[i] = cached->second;
            continue;
        }
        const Blob* mesh = find(env, s.shape + ".3DW");
        if (!mesh) {
            std::fprintf(stderr, "spot %s: no %s.3DW in the environment\n", s.shape.c_str(),
                         s.shape.c_str());
            continue;
        }
        ParticleKind kind;
        try {
            const ThreeDW m = parse_3dw(mesh->data(), mesh->size());
            const int layer = layer_for(m);
            if (layer < 0) continue; // no sheet: draw nothing rather than an untextured box
            const size_t frames = static_cast<size_t>(s.frame_count) + 1;
            kind.by_frame.resize(frames);
            kind.sprites_by_frame.resize(frames);
            for (size_t f = 0; f < frames; ++f) {
                kind.by_frame[f] = bake_fx_phase(m, layer, static_cast<int>(f));
                kind.sprites_by_frame[f] = bake_fx_sprites(m, layer, static_cast<int>(f));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "spot %s: %s\n", s.shape.c_str(), e.what());
            continue;
        }
        bool any = false;
        for (const std::vector<TerrainVertex>& v : kind.by_frame) any = any || !v.empty();
        for (const std::vector<ParticleSprite>& v : kind.sprites_by_frame) any = any || !v.empty();
        if (!any) continue;
        ++loaded;
        baked.emplace(key, kind);
        out.kinds[i] = std::move(kind);
    }
    if (loaded > 0) {
        std::printf("%d particle effect(s) baked from %zu spot line(s).\n", loaded, spots.size());
    }
    return out;
}

std::vector<TerrainVertex> place_particles(const ParticleVisuals& visuals,
                                           const ParticleField& field,
                                           const float right[3], const float up[3],
                                           int map_w, float render_scale) {
    std::vector<TerrainVertex> out;
    if (visuals.kinds.empty()) return out;
    const float mirror_axis =
        static_cast<float>(map_w - 1) * static_cast<float>(kTerrainTexelWorldSize);

    for (const Particle& p : field.slots()) {
        if (!p.alive()) continue;
        if (p.kind < 0 || static_cast<size_t>(p.kind) >= visuals.kinds.size()) continue;
        const ParticleKind& kind = visuals.kinds[static_cast<size_t>(p.kind)];
        const size_t f = static_cast<size_t>(p.frame);

        // The particle does not rotate: a crossed quad reads the same from every angle, which is
        // the whole reason the original authored them that way. So the placement is a
        // translation, through the same mirror and the same +Y flip as everything else.
        if (f < kind.by_frame.size()) {
            for (const TerrainVertex& v : kind.by_frame[f]) {
                TerrainVertex o = v;
                o.x = (mirror_axis - (v.x + static_cast<float>(p.x))) * render_scale;
                o.y = -(-v.y + static_cast<float>(p.y)) * render_scale; // mesh +Y up -> game down
                o.z = (v.z + static_cast<float>(p.z)) * render_scale;
                out.push_back(o);
            }
        }

        if (f >= kind.sprites_by_frame.size()) continue;
        for (const ParticleSprite& s : kind.sprites_by_frame[f]) {
            const float cx = (mirror_axis - (s.x + static_cast<float>(p.x))) * render_scale;
            const float cy = -(-s.y + static_cast<float>(p.y)) * render_scale;
            const float cz = (s.z + static_cast<float>(p.z)) * render_scale;
            const float hw = s.half_w * render_scale, hh = s.half_h * render_scale;
            static constexpr float kSx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            static constexpr float kSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
            static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            static constexpr float kV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            TerrainVertex corner[4];
            for (int i = 0; i < 4; ++i) {
                TerrainVertex v{};
                v.x = cx + right[0] * kSx[i] * hw + up[0] * kSy[i] * hh;
                v.y = cy + right[1] * kSx[i] * hw + up[1] * kSy[i] * hh;
                v.z = cz + right[2] * kSx[i] * hw + up[2] * kSy[i] * hh;
                v.u = s.u0 + (s.u1 - s.u0) * kU[i];
                v.v = s.v0 + (s.v1 - s.v0) * kV[i];
                v.layer = s.layer;
                v.has_texture = 1.0f;
                v.r = v.g = v.b = 1.0f;
                v.br = v.bg = v.bb = 1.0f;
                corner[i] = v;
            }
            out.push_back(corner[0]); out.push_back(corner[1]); out.push_back(corner[2]);
            out.push_back(corner[0]); out.push_back(corner[2]); out.push_back(corner[3]);
        }
    }
    return out;
}

} // namespace bigred
