#include "vehicle_render.h"

#include "../formats/terrain.h"
#include "../formats/texture.h" // kTmWidth/kTmHeight, decode_tm_rgba - crops are in raw texels
#include "../formats/shading.h" // DEFAULT.CG - what the lighting walks an index up

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace bigred {
namespace {

// A shape's faces, fan-triangulated into local render space. Same conventions as the decor
// builder in level_scene.cpp: .3DW vertices are stored at 2x and with Y inverted, so halve and
// negate. A face with no resolvable palette colour is a logo slot - the player's chosen livery
// in the original. Until that is a choice here, it takes the body's own darkest ramp step so the
// car reads as one object instead of flashing magenta.
// Which step of THIS REPAINT's block a palette index is, or -1 when it is not this vehicle's
// team colour. See vehicle_render.h: the other five blocks are livery, not team paint, and the
// first two offsets of every block are shared trim.
int team_ramp_step(int index, int colour) {
    if (colour < 1 || colour > kTeamCount) colour = 1;
    const int off = index - (kTeamRampFirst + (colour - 1) * kTeamRampStride);
    if (off < kTeamRampFirstPainted || off >= kTeamRampSteps) return -1;
    return off;
}

// The colour a palette index paints a vehicle face with: the palette's own, or - under a tint -
// the same STEP of the player's regenerated ramp. Shared by the build and by the lighting, which
// walks the index up DEFAULT.CG first and then asks the same question.
void face_rgb(int ci, const std::vector<RGB>& palette, const VehicleTint* tint, int colour,
              float& r, float& g, float& b) {
    r = 0.45f; g = 0.06f; b = 0.05f;
    if (ci < 0 || static_cast<size_t>(ci) >= palette.size()) return;
    const RGB& c = palette[static_cast<size_t>(ci)];
    r = c.r / 255.0f; g = c.g / 255.0f; b = c.b / 255.0f;
    // The player's own colour, walked down the same ramp the artists drew. The STEP is kept - it
    // is the fake shading, and it is the only thing the face index encodes that must survive a
    // recolour.
    if (tint) {
        const int step = team_ramp_step(ci, colour);
        if (step >= 0) {
            const float f = 1.0f - static_cast<float>(step - kTeamRampFirstPainted) *
                                       (1.0f - kTeamRampFloor) /
                                       static_cast<float>(kTeamRampSteps - 1 - kTeamRampFirstPainted);
            r = static_cast<float>(tint->r) / 255.0f * f;
            g = static_cast<float>(tint->g) / 255.0f * f;
            b = static_cast<float>(tint->b) / 255.0f * f;
        }
    }
}

// A shape's TEXTURED faces, by exactly the test the decor uses (level_scene.cpp's
// `is_texture_path_face`). It is the one rule that tells a Beetle's wheel RIM from a body's
// LOGO SLOT, and both are "a face with no palette colour":
//
//   * a shape that carries UV records (`texture_variants`) paints its colourless faces with
//     them - that is the rim, the hubcap, the dashboard;
//   * a shape with none has nothing to paint them with, so a colourless face there really is
//     the player's logo slot, and stays flat until something draws a logo on it.
//
// ⚠️ THIS IS WHY WHEELS HAD RED SQUARES FOR RIMS. `shape_to_local` read `face.color` and
// nothing else, so every colourless face fell to the flat fallback - a dark red quad where
// `BTWH.TM` (Beetle), `MWHL.TM` (Mini) or a dashboard belonged. Reported by the user,
// 2026-09-23: "des carres rouges a la place des jantes".
// The emblem, straight into the sheet's own cell. Both are RGBA and the cell is exactly the
// emblem's size, so this is a row-by-row copy and nothing else - the transparent surround of the
// emblem stays transparent, which is what lets the bodywork show round it.
void paint_logo_cell(std::vector<uint8_t>& sheet_rgba, const VehicleLogo& logo, int colour) {
    if (sheet_rgba.size() != kTmSize * 4 || !logo.valid()) return;
    int cx = 0, cy = 0;
    logo_cell_origin(colour, cx, cy);
    for (int row = 0; row < kLogoCell; ++row) {
        const int dy = cy + row;
        if (dy < 0 || dy >= kTmHeight) continue;
        uint8_t* dst = sheet_rgba.data() + (static_cast<size_t>(dy) * kTmWidth + cx) * 4;
        const uint8_t* src = logo.rgba + static_cast<size_t>(row) * kLogoCell * 4;
        std::copy(src, src + static_cast<size_t>(kLogoCell) * 4, dst);
    }
}

// How far a decal stands off the panel it is painted on, in the vehicle's own local units - a
// car body is some 200 of them across, so this is well under a millimetre at that scale and far
// more than a depth buffer needs to tell two surfaces apart.
constexpr float kDecalLift = 1.0f;

bool is_texture_path_face(const ThreeDW& model, const Face& face) {
    return face.textured_variant || (!model.texture_variants.empty() && !face.color.has_value());
}

// `eye`, when given, is where the shape is seen FROM, in its local render space (+Y up): the
// cabin's. See the decal lift below.
std::vector<TerrainVertex> shape_to_local(const ThreeDW& model, const std::vector<RGB>& palette,
                                          const std::vector<RGB>& palette_bright,
                                          const VehicleTint* tint, int layer, int colour,
                                          const float* eye = nullptr,
                                          std::vector<VertexShade>* shade_out = nullptr) {
    std::vector<TerrainVertex> out;
    // THE LIGHTING'S NORMALS: per vertex, the average of the
    // normals of the faces around it, as the original builds them at load - in the
    // shape's game axes, which are the .3DW's own (x, y, z) halved. Each face normal is turned
    // to the side that is SEEN - away from the shape's origin, or towards the eye in a cabin -
    // rather than trusted to a winding this port has never needed to settle.
    std::vector<std::array<float, 3>> vnormal;
    if (shade_out) {
        vnormal.assign(model.vertices.size(), {0.0f, 0.0f, 0.0f});
        const auto at = [&](int32_t i) -> std::array<float, 3> {
            if (i < 0 || static_cast<size_t>(i) >= model.vertices.size()) return {0, 0, 0};
            const Vec3i& p = model.vertices[static_cast<size_t>(i)];
            return {static_cast<float>(p.x) * kModelVertexScale,
                    static_cast<float>(p.y) * kModelVertexScale,
                    static_cast<float>(p.z) * kModelVertexScale};
        };
        for (const Face& f : model.faces) {
            if (f.indices.size() < 3) continue;
            const std::array<float, 3> a = at(f.indices[0]), b = at(f.indices[1]), c = at(f.indices[2]);
            float n[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                          (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                          (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-4f) continue;
            float cx = 0, cy = 0, cz = 0;
            for (const int32_t i : f.indices) {
                const std::array<float, 3> p = at(i);
                cx += p[0]; cy += p[1]; cz += p[2];
            }
            const float inv = 1.0f / static_cast<float>(f.indices.size());
            cx *= inv; cy *= inv; cz *= inv;
            // `eye` is in local RENDER space (+Y up): back to game axes for the test.
            const float side = eye ? n[0] * (eye[0] - cx) + n[1] * (-eye[1] - cy) + n[2] * (eye[2] - cz)
                                   : n[0] * cx + n[1] * cy + n[2] * cz;
            const float s = (side < 0.0f ? -1.0f : 1.0f) / len;
            for (const int32_t i : f.indices) {
                if (i < 0 || static_cast<size_t>(i) >= vnormal.size()) continue;
                for (int k = 0; k < 3; ++k) vnormal[static_cast<size_t>(i)][k] += n[k] * s;
            }
        }
        for (std::array<float, 3>& v : vnormal) {
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-6f) for (float& x : v) x /= len;
        }
    }
    // Which crop each face ends up with, when it has no UV of its own. The same replay the
    // effect flipbooks use (bake_fx_phase), at variant 0: a vehicle has no `MAP.DAT` byte to
    // select an alternative with, so it is always the display list's own first answer.
    //
    // ⚠️ `replay.drawn` IS DELIBERATELY IGNORED HERE. A body's display list can branch on
    // damage state, and honouring it at variant 0 would silently delete bodywork; this pass only
    // wants the crop each face inherits. Nothing is added or removed, only painted.
    DisplayListReplay replay;
    if (layer >= 0)
        replay = replay_display_list(model.branches, model.face_stream_pos, model.crop_stream_pos,
                                     model.faces.size(), 0);

    // Every FLAT face's vertex set, so a textured face laid on the same corners can be
    // recognised for what it is: a DECAL.
    //
    // ⚠️ THE DECOR'S RULE IS THE WRONG WAY ROUND HERE, and getting that backwards punches holes
    // in a car. On decor, a textured face covers the flat one and the flat one is dropped
    // (level_scene.cpp's `matches_textured_face`). On a vehicle the logo cell's surround is
    // palette index 0 - TRANSPARENT, measured on JDSH/BDSH/HDSH - so the emblem is painted OVER
    // the bodywork panel and needs it to stay there. Drop the panel and you see through the
    // nose of the car.
    //
    // ⚠️ AND BOTH AT THE SAME DEPTH FIGHT. The original draws in queue order and never needed
    // to separate them; we have a depth buffer. The decal is lifted a hair off the panel, the
    // same answer the decor's coplanar lifts give. The user reported the flicker as "une
    // bataille de texture ou de face a l'avant" on the buggy, 2026-09-23.
    std::vector<std::vector<int32_t>> flat_sets;
    if (layer >= 0) {
        for (const Face& f : model.faces) {
            if (f.indices.size() < 3 || is_texture_path_face(model, f)) continue;
            std::vector<int32_t> key = f.indices;
            std::sort(key.begin(), key.end());
            flat_sets.push_back(std::move(key));
        }
    }
    const auto is_decal = [&](const Face& f) {
        if (flat_sets.empty()) return false;
        std::vector<int32_t> key = f.indices;
        std::sort(key.begin(), key.end());
        for (const std::vector<int32_t>& s : flat_sets)
            if (s == key) return true;
        return false;
    };

    for (size_t fi = 0; fi < model.faces.size(); ++fi) {
        const Face& face = model.faces[fi];
        if (face.indices.size() < 3) continue;
        if (layer >= 0 && is_texture_path_face(model, face)) {
            // ⚠️ A TEXTURED FACE THE WALK NEVER REACHES IS NOT DRAWN AT ALL - unlike a flat one,
            // where the replay is deliberately ignored (see below). The hovercraft's rudder is
            // exactly this: face 16 is its emblem and face 33 is a second copy the display list
            // steps over, and drawing both put two quads on the same corners on each side. The
            // user reported it the day the texture path landed.
            if (fi < replay.drawn.size() && !replay.drawn[fi]) continue;
            const bool own_uv = face.uv.size() == face.indices.size();
            const UVVariant* crop = nullptr;
            if (!own_uv) {
                const int ci = (fi < replay.crop.size()) ? replay.crop[fi] : -1;
                if (ci >= 0 && static_cast<size_t>(ci) < model.texture_variants.size())
                    crop = &model.texture_variants[static_cast<size_t>(ci)];
            }
            if (own_uv || crop) {
                // ⚠️ THE CROP'S OWN CORNER LIST CARRIES THE ORIENTATION; its bounding rect does
                // not. Same trap as the decor's and the flame's - see bake_fx_phase.
                const std::vector<std::array<float, 2>>* corner_uv =
                    (crop && crop->uv.size() == face.indices.size()) ? &crop->uv : nullptr;
                static constexpr float kCornerU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                static constexpr float kCornerV[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                std::vector<TerrainVertex> corner;
                corner.reserve(face.indices.size());
                // ⚠️ A FACE THAT SAMPLES THE EMBLEM GRID IS A DECAL, whatever its corners are.
                // Matching vertex SETS is not enough on its own: a hovercraft rudder is sixteen
                // faces a side, so its emblem quad shares its four corners with no single flat
                // face and the vertex test misses it entirely - the emblem then fights the fin
                // it is painted on. Where a face samples is the reliable signal, and the grid
                // is exactly the bottom-right corner of the sheet.
                bool decal = is_decal(face);
                for (size_t i = 0; i < face.indices.size(); ++i) {
                    const int32_t idx = face.indices[i];
                    TerrainVertex v{};
                    if (static_cast<size_t>(idx) < model.vertices.size()) {
                        const Vec3i& p = model.vertices[static_cast<size_t>(idx)];
                        constexpr float s = kModelVertexScale;
                        v.x = static_cast<float>(p.x) * s;
                        v.y = -static_cast<float>(p.y) * s; // .3DW Y is inverted
                        v.z = static_cast<float>(p.z) * s;
                    }
                    if (own_uv) {
                        v.u = face.uv[i][0] / kTmWidth;
                        v.v = face.uv[i][1] / kTmHeight;
                    } else if (corner_uv) {
                        v.u = (*corner_uv)[i][0] / kTmWidth;
                        v.v = (*corner_uv)[i][1] / kTmHeight;
                    } else {
                        const size_t k = i % 4;
                        v.u = (crop->u0 + (crop->u1 - crop->u0) * kCornerU[k]) / kTmWidth;
                        v.v = (crop->v0 + (crop->v1 - crop->v0) * kCornerV[k]) / kTmHeight;
                    }
                    v.layer = static_cast<float>(layer);
                    v.has_texture = 1.0f;
                    v.r = v.g = v.b = 1.0f;
                    v.br = v.bg = v.bb = 1.0f;
                    corner.push_back(v);
                }
                if (!decal) {
                    float min_u = 1e9f, min_v = 1e9f;
                    for (const TerrainVertex& v : corner) {
                        min_u = std::min(min_u, v.u * kTmWidth);
                        min_v = std::min(min_v, v.v * kTmHeight);
                    }
                    decal = min_u >= static_cast<float>(kLogoGridX) - 0.5f &&
                            min_v >= static_cast<float>(kLogoGridY) - 0.5f;
                }
                // The lift, along the face's own normal and AWAY FROM THE BODY. Which way that
                // is cannot come from the winding - nothing here is culled - so it is taken from
                // the face's position relative to the vehicle's own origin, which is inside it.
                // ⚠️ A CABIN IS SEEN FROM INSIDE: there the lift goes TOWARDS the eye. Away from
                // the origin, the dashboard's dials sank behind their own grey panel and the
                // cabin view showed a blank slab where the gauges are.
                if (decal && corner.size() >= 3) {
                    const float ax = corner[1].x - corner[0].x, ay = corner[1].y - corner[0].y,
                                az = corner[1].z - corner[0].z;
                    const float bx = corner[2].x - corner[0].x, by = corner[2].y - corner[0].y,
                                bz = corner[2].z - corner[0].z;
                    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-4f) {
                        nx /= len; ny /= len; nz /= len;
                        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                        for (const TerrainVertex& v : corner) { cx += v.x; cy += v.y; cz += v.z; }
                        const float inv = 1.0f / static_cast<float>(corner.size());
                        cx *= inv; cy *= inv; cz *= inv;
                        const bool flip = eye ? nx * (eye[0] - cx) + ny * (eye[1] - cy) +
                                                        nz * (eye[2] - cz) < 0.0f
                                              : nx * cx + ny * cy + nz * cz < 0.0f;
                        if (flip) { nx = -nx; ny = -ny; nz = -nz; }
                        for (TerrainVertex& v : corner) {
                            v.x += nx * kDecalLift;
                            v.y += ny * kDecalLift;
                            v.z += nz * kDecalLift;
                        }
                    }
                }
                for (size_t i = 1; i + 1 < corner.size(); ++i) {
                    out.push_back(corner[0]);
                    out.push_back(corner[i]);
                    out.push_back(corner[i + 1]);
                }
                continue;
            }
        }
        const int ci = (face.color.has_value() && *face.color >= 0 &&
                        static_cast<size_t>(*face.color) < palette.size())
                           ? *face.color
                           : -1;
        float r, g, b;
        face_rgb(ci, palette, tint, colour, r, g, b);
        // ⚠️ A VEHICLE IS NOT DISTANCE-SHADED, on purpose. The world's shader walks each face up
        // its palette ramp as it nears the camera, and a chase
        // camera keeps the car pinned at distance ~0 - so it would always render at the bright
        // end. That is precisely what must not happen here: a car body's face indices ALREADY
        // ARE a 7-step luminance ramp of its team colour, baked by the artists to fake
        // per-facet shading. Re-lighting them erases the very
        // thing they encode. Passing the authored colour as both ends makes the shader's `mix`
        // a no-op, with no special case in the shared shader.
        const float br = r, bg = g, bb = b;
        auto vert = [&](int32_t idx) -> TerrainVertex {
            TerrainVertex v{};
            if (static_cast<size_t>(idx) >= model.vertices.size()) return v;
            const Vec3i& p = model.vertices[static_cast<size_t>(idx)];
            constexpr float s = kModelVertexScale;
            v.x = static_cast<float>(p.x) * s;
            v.y = -static_cast<float>(p.y) * s; // .3DW Y is inverted
            v.z = static_cast<float>(p.z) * s;
            v.r = r; v.g = g; v.b = b;
            v.br = br; v.bg = bg; v.bb = bb;
            v.u = v.v = v.layer = v.has_texture = 0.0f;
            return v;
        };
        const auto push = [&](int32_t idx) {
            out.push_back(vert(idx));
            if (!shade_out) return;
            shade_out->resize(out.size() - 1);   // the textured faces before: not lit
            VertexShade s;
            if (idx >= 0 && static_cast<size_t>(idx) < vnormal.size()) {
                s.nx = vnormal[static_cast<size_t>(idx)][0];
                s.ny = vnormal[static_cast<size_t>(idx)][1];
                s.nz = vnormal[static_cast<size_t>(idx)][2];
                s.idx = ci;
            }
            shade_out->push_back(s);
        };
        for (size_t i = 1; i + 1 < face.indices.size(); ++i) {
            push(face.indices[0]);
            push(face.indices[i]);
            push(face.indices[i + 1]);
        }
    }
    if (shade_out) shade_out->resize(out.size());
    return out;
}

const Blob* find(const std::unordered_map<std::string, Blob>& a, const std::string& name) {
    auto it = a.find(name);
    return it == a.end() ? nullptr : &it->second;
}

} // namespace

// One phase of an effect flipbook, in local render space. See VehicleFx: the shape's faces
// carry no UV of their own, and which CROP of the sprite sheet each one gets is decided by
// replaying the display list with the animation counter as the variant byte.
//
// Shared with the ground-effect particles (scene/particle_render.cpp): NIT_FX, DUST1_FX,
// SPLASH2 and the rest are the same construction, and were always meant to bake the same way.
std::vector<TerrainVertex> bake_fx_phase(const ThreeDW& model, int layer, int phase) {
    std::vector<TerrainVertex> out;
    const DisplayListReplay replay =
        replay_display_list(model.branches, model.face_stream_pos, model.crop_stream_pos,
                            model.faces.size(), phase);
    for (size_t fi = 0; fi < model.faces.size(); ++fi) {
        if (fi < replay.drawn.size() && !replay.drawn[fi]) continue;
        const Face& face = model.faces[fi];
        if (face.indices.size() < 3) continue;

        const bool own_uv = face.uv.size() == face.indices.size();
        const UVVariant* crop = nullptr;
        if (!own_uv) {
            const int c = (fi < replay.crop.size()) ? replay.crop[fi] : -1;
            if (c >= 0 && static_cast<size_t>(c) < model.texture_variants.size())
                crop = &model.texture_variants[static_cast<size_t>(c)];
        }
        // No crop reached this face on this phase: the tree meant to draw nothing here. Drawing
        // it untextured is exactly the bug this replaces - an untextured tag=6 face has no
        // colour either, so it comes out as the flat fallback.
        if (!own_uv && !crop) continue;
        // ⚠️ The crop's OWN corner list carries the orientation; its bounding rect does not.
        // Same trap as the decor's - see level_scene.cpp and ROADSIGN's mirrored arrows.
        const std::vector<std::array<float, 2>>* corner_uv =
            (crop && crop->uv.size() == face.indices.size()) ? &crop->uv : nullptr;
        static constexpr float kCornerU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static constexpr float kCornerV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

        std::vector<TerrainVertex> corner;
        corner.reserve(face.indices.size());
        for (size_t i = 0; i < face.indices.size(); ++i) {
            const int32_t idx = face.indices[i];
            TerrainVertex v{};
            if (static_cast<size_t>(idx) < model.vertices.size()) {
                const Vec3i& pt = model.vertices[static_cast<size_t>(idx)];
                constexpr float sc = kModelVertexScale;
                v.x = static_cast<float>(pt.x) * sc;
                v.y = -static_cast<float>(pt.y) * sc; // .3DW Y is inverted
                v.z = static_cast<float>(pt.z) * sc;
            }
            if (own_uv) {
                v.u = face.uv[i][0] / kTmWidth;
                v.v = face.uv[i][1] / kTmHeight;
            } else if (corner_uv) {
                v.u = (*corner_uv)[i][0] / kTmWidth;
                v.v = (*corner_uv)[i][1] / kTmHeight;
            } else {
                const size_t c = i % 4;
                v.u = (crop->u0 + (crop->u1 - crop->u0) * kCornerU[c]) / kTmWidth;
                v.v = (crop->v0 + (crop->v1 - crop->v0) * kCornerV[c]) / kTmHeight;
            }
            v.layer = static_cast<float>(layer);
            v.has_texture = 1.0f;
            // The shader mixes vColor/vColorLit only where has_texture is 0, and the atlas's
            // lit half for this layer is a copy of its raw half (add_atlas_texture's `emissive`
            // flag), so nothing here can be walked up a ramp. A flame lights the scene; it is
            // not lit by it.
            v.r = v.g = v.b = 1.0f;
            v.br = v.bg = v.bb = 1.0f;
            corner.push_back(v);
        }
        for (size_t i = 1; i + 1 < corner.size(); ++i) {
            out.push_back(corner[0]);
            out.push_back(corner[i]);
            out.push_back(corner[i + 1]);
        }
    }
    return out;
}

// The `tag=13` billboards a flipbook frame reaches, resolved into render space.
//
// ⚠️ `width`/`height` are HALF-extents ALREADY IN TRUE UNITS: kModelVertexScale applies to the
// anchor VERTEX and not to them. Getting that wrong is what made the destructibles' filmstrips
// come out half size.
std::vector<ParticleSprite> bake_fx_sprites(const ThreeDW& model, int layer, int phase) {
    std::vector<ParticleSprite> out;
    if (model.sprites.empty()) return out;
    std::vector<int32_t> spos;
    spos.reserve(model.sprites.size());
    for (const Sprite& s : model.sprites) spos.push_back(s.stream_pos);
    const DisplayListReplay replay =
        replay_display_list(model.branches, model.face_stream_pos, model.crop_stream_pos,
                            model.faces.size(), phase, spos);
    for (size_t i = 0; i < model.sprites.size(); ++i) {
        if (i >= replay.sprite_drawn.size() || !replay.sprite_drawn[i]) continue;
        const int c = replay.sprite_crop[i];
        if (c < 0 || static_cast<size_t>(c) >= model.texture_variants.size()) continue;
        const Sprite& sp = model.sprites[i];
        if (sp.vertex < 0 || static_cast<size_t>(sp.vertex) >= model.vertices.size()) continue;
        const Vec3i& v = model.vertices[static_cast<size_t>(sp.vertex)];
        const UVVariant& uv = model.texture_variants[static_cast<size_t>(c)];
        ParticleSprite ps;
        constexpr float sc = kModelVertexScale;
        ps.x = static_cast<float>(v.x) * sc;
        ps.y = -static_cast<float>(v.y) * sc; // .3DW Y is inverted
        ps.z = static_cast<float>(v.z) * sc;
        ps.half_w = static_cast<float>(sp.width);
        ps.half_h = static_cast<float>(sp.height);
        ps.u0 = uv.u0 / kTmWidth;
        ps.v0 = uv.v0 / kTmHeight;
        ps.u1 = uv.u1 / kTmWidth;
        ps.v1 = uv.v1 / kTmHeight;
        ps.layer = static_cast<float>(layer);
        out.push_back(ps);
    }
    return out;
}

// The one builder both entry points below use. `ingame` and `scene` are NULL for a menu: there
// is no atlas to append an effect's sprite sheet to, so the emitters are left out and every
// other piece is built identically. Nothing else differs between the two callers.
static VehicleModel build_model(const std::unordered_map<std::string, Blob>& vhc,
                                const std::unordered_map<std::string, Blob>* ingame,
                                const VehicleTuning& tune, int colour,
                                const std::vector<RGB>& palette,
                                const std::vector<RGB>& palette_bright, LevelScene* scene,
                                const VehicleTint* tint, const TextureSink& add_texture,
                                const VehicleLogo* logo, const VehicleDriver* driver,
                                int section = 0) {
    VehicleModel out;
    if (section < 0 || static_cast<size_t>(section) >= tune.sections.size()) return out;
    const VehicleSection& sec = tune.sections[static_cast<size_t>(section)]; // [1]: the cabin
    if (colour < 1 || colour > 6) colour = 1;
    // ⚠️ A TINT MAKES THE REPAINT IRRELEVANT, so variant 1 is taken and the ramp rewritten. Any
    // of the six would do - the remap catches all six blocks - but picking one keeps "the
    // canonical geometry" a single, nameable thing.
    if (tint) colour = 1;

    // A shape names the texture it wants INSIDE ITSELF - never by its own filename, the same
    // rule the decor follows (formats/threedw.h). Resolved once per name: a vehicle's wheels all
    // share one sheet.
    std::unordered_map<std::string, int> tex_layer;
    auto layer_for_shape = [&](const ThreeDW& m) -> int {
        if (!add_texture || m.embedded_texture_name.empty()) return -1;
        if (auto it = tex_layer.find(m.embedded_texture_name); it != tex_layer.end())
            return it->second;
        const Blob* tm = find(vhc, m.embedded_texture_name + ".TM");
        int layer = -1;
        if (tm && tm->size() == kTmSize) {
            std::vector<uint8_t> rgba = decode_tm_rgba(tm->data(), palette);
            // The player's emblem, into the cell this repaint's logo slots sample. Exactly what
            // the original does, and the reason nothing downstream has to know about logos.
            if (logo && logo->valid()) paint_logo_cell(rgba, *logo, colour);
            layer = add_texture(std::move(rgba));
        }
        tex_layer.emplace(m.embedded_texture_name, layer);
        return layer;
    };

    const Blob* body = find(vhc, sec.base + std::to_string(colour) + ".3DW");
    if (!body) body = find(vhc, sec.base + "1.3DW");
    if (!body) return out;
    try {
        const ThreeDW m = parse_3dw(body->data(), body->size());
        // The cabin is looked at from its `internal` eye: game axes (+Y down) to local render.
        const float eye[3] = {static_cast<float>(sec.internal[0]),
                              -static_cast<float>(sec.internal[1]),
                              static_cast<float>(sec.internal[2])};
        out.body = shape_to_local(m, palette, palette_bright, tint, layer_for_shape(m), colour,
                                  section == 1 ? eye : nullptr, &out.body_shade);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vehicle body %s: %s\n", sec.base.c_str(), e.what());
        return out;
    }

    // The flame's sprite sheet, appended to the level's atlas once and shared by every emitter.
    // Resolved from the shape's OWN embedded texture name (NIT_FX embeds "SPOTFX3"), never from
    // the shape's filename - the same rule the decor follows, see threedw.h.
    std::unordered_map<std::string, int> fx_layer;
    auto layer_for = [&](const ThreeDW& m) -> int {
        if (m.embedded_texture_name.empty()) return -1;
        auto it = fx_layer.find(m.embedded_texture_name);
        if (it != fx_layer.end()) return it->second;
        const Blob* tm = find(*ingame, m.embedded_texture_name + ".TM");
        const int layer = tm ? scene->add_atlas_texture(*tm, /*emissive=*/true) : -1;
        fx_layer.emplace(m.embedded_texture_name, layer);
        return layer;
    };

    for (const WheelAttach& w : sec.wheels) {
        // &87/&88 is the nitro flame emitter. Its shape is never in the `.VHC`: NIT_FX, FLAMES,
        // SMOKE and BOOM all live in INGAME.WAP, which the original keeps loaded at all times
        // ("always loaded, so not in the .BAT", says JEEP.TXT itself).
        if (ingame && scene && (w.flags == 0x87 || w.flags == 0x88)) {
            const Blob* fxmesh = find(*ingame, w.shape.empty() ? std::string("NIT_FX.3DW")
                                                              : w.shape + ".3DW");
            if (!fxmesh) fxmesh = find(*ingame, "NIT_FX.3DW");
            if (!fxmesh) continue;
            VehicleFx fxp;
            try {
                const ThreeDW m = parse_3dw(fxmesh->data(), fxmesh->size());
                const int layer = layer_for(m);
                if (layer < 0) continue; // no sheet, no flame - better than a red box
                fxp.by_phase.resize(kFlamePhaseCount);
                for (int phase = 0; phase < kFlamePhaseCount; ++phase)
                    fxp.by_phase[static_cast<size_t>(phase)] = bake_fx_phase(m, layer, phase);
            } catch (const std::exception&) {
                continue;
            }
            bool any = false;
            for (const std::vector<TerrainVertex>& v : fxp.by_phase) any = any || !v.empty();
            if (!any) continue;
            fxp.pos[0] = w.pos[0];
            fxp.pos[1] = w.pos[1];
            fxp.pos[2] = w.pos[2];
            fxp.yaw_bam16 = w.rot[1];
            out.nitro_fx.push_back(std::move(fxp));
            continue;
        }
        // &89..&8D, the damage effects: FLAMES, SMOKE x3, BOOM - INGAME.WAP too.
        if (ingame && scene && w.flags >= 0x89 && w.flags <= 0x8D) {
            const char* fallback = w.flags == 0x89 ? "FLAMES" : (w.flags == 0x8D ? "BOOM" : "SMOKE");
            const Blob* fxmesh = find(*ingame, w.shape.empty() ? std::string(fallback) + ".3DW"
                                                              : w.shape + ".3DW");
            if (!fxmesh) fxmesh = find(*ingame, std::string(fallback) + ".3DW");
            if (!fxmesh) continue;
            VehicleDamageFx d;
            d.flags = w.flags;
            try {
                const ThreeDW m = parse_3dw(fxmesh->data(), fxmesh->size());
                const int layer = layer_for(m);
                if (layer < 0) continue;
                d.by_phase.resize(kDamageFxPhases);
                d.sprites_by_phase.resize(kDamageFxPhases);
                for (int phase = 0; phase < kDamageFxPhases; ++phase) {
                    d.by_phase[static_cast<size_t>(phase)] = bake_fx_phase(m, layer, phase);
                    d.sprites_by_phase[static_cast<size_t>(phase)] = bake_fx_sprites(m, layer, phase);
                }
            } catch (const std::exception&) {
                continue;
            }
            for (int i = 0; i < 3; ++i) d.pos[i] = w.pos[i];
            out.damage_fx.push_back(std::move(d));
            continue;
        }
        // Everything else that names a shape of this `.VHC` is a piece of the vehicle and gets
        // drawn. Only the effect emitters are skipped here: their shapes are not in the archive.
        //
        // `&FF` is a decorative piece bolted to the body - the SNOWCAT's two tracks
        // (`CATTRAKL`/`CATTRAKR`), the PLOUGH's (`TRACKL`, twice, the second one yawed half a
        // turn to mirror it), the JEEP's spare wheel. `&80`/`&81`/`&83` are the powered parts -
        // the helicopters' two rotors, the UFO's counter-rotating pair, the hovercraft's fan and
        // rudders. ⚠️ THEY ARE DRAWN, NOT ANIMATED: nothing turns yet. Skipping every non-wheel
        // anchor is what left those vehicles bald. The driver's head stays missing, and for
        // another reason: its shape is `&40`, the global table, i.e. the character picked.
        if (w.is_effect()) continue;
        // The driver's head: the anchor names no shape of this `.VHC` - `&40` is the global
        // table, the character the player picked. Built with the CHARACTER's colour and the
        // mouth sheet that ships with the heads.
        if (w.shape.empty() && w.is_decor() && (w.shape_sel & 0x40) != 0 && driver &&
            driver->head) {
            int mouth_layer = -1;
            if (add_texture && driver->mouth_tm && driver->mouth_tm->size() == kTmSize)
                mouth_layer = add_texture(decode_tm_rgba(driver->mouth_tm->data(), palette));
            VehiclePart part;
            part.local = shape_to_local(*driver->head, palette, palette_bright, driver->tint,
                                        mouth_layer, 1);
            if (part.local.empty()) continue;
            for (int i = 0; i < 3; ++i) {
                part.pos[i] = w.pos[i];
                part.rot_bam16[i] = w.rot[i];
            }
            part.flags = w.flags;
            out.parts.push_back(std::move(part));
            continue;
        }
        if (w.shape.empty()) continue;
        // A part that owns the team variants follows the body's colour, like the body itself.
        std::string shape_name = w.shape;
        if (w.coloured && !shape_name.empty())
            shape_name[shape_name.size() - 1] = static_cast<char>('0' + colour);
        const Blob* mesh = find(vhc, shape_name + ".3DW");
        if (!mesh) mesh = find(vhc, w.shape + ".3DW");
        if (!mesh) continue;
        VehiclePart part;
        try {
            const ThreeDW m = parse_3dw(mesh->data(), mesh->size());
            part.local = shape_to_local(m, palette, palette_bright, tint, layer_for_shape(m),
                                        colour, nullptr, &part.shade);
        } catch (const std::exception&) {
            continue;
        }
        if (part.local.empty()) continue;
        part.pos[0] = w.pos[0];
        part.pos[1] = w.pos[1];
        part.pos[2] = w.pos[2];
        for (int i = 0; i < 3; ++i) part.rot_bam16[i] = w.rot[i];
        part.flags = w.flags;
        part.powered = w.is_rotor();
        part.steer_gain = w.steer_bam_per_unit();
        if (w.flags == 0x82) part.roll_steer_gain = -w.size;
        part.rolls = w.is_wheel(); // a track or a spare wheel hangs still
        out.parts.push_back(std::move(part));
    }

    // THE WRECK: `vehicle <base> 7` ships a seventh shape, `<base>X`, which the original draws
    // in place of the body once the car is destroyed. Six shapes: the painted body stays.
    if (sec.count >= 7) {
        if (const Blob* x = find(vhc, sec.base + "X.3DW")) {
            try {
                const ThreeDW m = parse_3dw(x->data(), x->size());
                out.wreck_body = shape_to_local(m, palette, palette_bright, tint, layer_for_shape(m), colour);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "vehicle wreck %sX: %s\n", sec.base.c_str(), e.what());
            }
        }
    }
    for (const TerrainVertex& v : out.body)
        out.radius = std::max(out.radius, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    // And where each part was thrown: three random angles apiece, fixed for the model.
    uint32_t dice = 0x9e3779b9u ^ static_cast<uint32_t>(sec.base.size() * 2654435761u);
    for (char c : sec.base) dice = dice * 31u + static_cast<uint8_t>(c);
    out.wreck_rot.resize(out.parts.size());
    for (std::array<int32_t, 3>& r : out.wreck_rot)
        for (int32_t& a : r) {
            dice = dice * 1103515245u + 12345u;
            a = static_cast<int32_t>((dice >> 16) & 0xFFFF);
        }
    out.sheet_layers = tex_layer;
    // The lighting's colours: the race only (a menu passes no scene, hence no DEFAULT.CG).
    if (scene && scene->shade_table.size() == kShadeTableSize) {
        out.shade_palette = palette;
        out.shade_cg = &scene->shade_table;
        out.shade_tinted = tint != nullptr;
        if (tint) out.shade_tint = *tint;
        out.shade_colour = colour;
    }
    return out;
}

void sun_direction(const SunLight& s, float out[3]) {
    constexpr float kBamToRad = 6.283185307179586f / 65536.0f;
    const float a = static_cast<float>(s.elevation) * kBamToRad;
    const float b = static_cast<float>(s.azimuth) * kBamToRad;
    out[0] = -std::cos(a) * std::sin(b);
    out[1] = -std::sin(a);
    out[2] = -std::cos(a) * std::cos(b);
}

int light_intensity(float distance) {
    // The original's curve, `((cells - 2) * 0x200 - depth) >> 4`. `cells` is the original's
    // draw distance in cells, retuned every frame between 10 and 22 against the frame rate; 16,
    // the middle, is a choice (HYPOTHESE): full light out to ~3 000 units, none past 7 168.
    constexpr int kLightCells = 16;
    const int v = (static_cast<int>((kLightCells - 2) * 0x200 - distance)) >> 4;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void logo_cell_origin(int colour, int& x, int& y) {
    if (colour < 1 || colour > kTeamCount) colour = 1;
    const int n = colour - 1;
    x = kLogoGridX + (n % kLogoGridCols) * kLogoCell;
    y = kLogoGridY + (n / kLogoGridCols) * kLogoCell;
}

VehicleModel build_vehicle_model(const std::unordered_map<std::string, Blob>& vhc,
                                 const std::unordered_map<std::string, Blob>& ingame,
                                 const VehicleTuning& tune, int colour, LevelScene& scene,
                                 const VehicleTint* tint, const VehicleLogo* logo,
                                 const VehicleDriver* driver, int section) {
    // ⚠️ EMISSIVE, like the flame's sheet and for the same reason: a vehicle is never
    // distance-shaded (see shape_to_local), so its lit half must be a copy of its raw half, or a
    // chase camera - which keeps the car at distance zero - would light every wheel rim to the
    // bright end of a ramp it has no business being on.
    return build_model(vhc, &ingame, tune, colour, scene.palette, scene.palette_bright, &scene,
                       tint, [&scene](std::vector<uint8_t> rgba) {
                           return scene.add_atlas_rgba(std::move(rgba));
                       }, logo, driver, section);
}

VehicleModel build_vehicle_model(const std::unordered_map<std::string, Blob>& vhc,
                                 const VehicleTuning& tune, int colour,
                                 const std::vector<RGB>& palette, const VehicleTint* tint,
                                 const TextureSink& add_texture, const VehicleLogo* logo,
                                 const VehicleDriver* driver, int section) {
    // The bright end IS the raw colour here, and that is not an approximation: shape_to_local
    // passes the authored colour as both ends whatever it is given, because a vehicle is never
    // distance-shaded (see the note there). A second palette would change nothing.
    return build_model(vhc, nullptr, tune, colour, palette, palette, nullptr, tint, add_texture,
                       logo, driver, section);
}

std::vector<TerrainVertex> shape_mesh(const ThreeDW& model, const std::vector<RGB>& palette,
                                      const VehicleTint* tint, int layer) {
    // The raw colour at both ends, as for a vehicle in a menu: nothing here is distance-shaded.
    return shape_to_local(model, palette, palette, tint, layer, 1);
}

// Which frame of a damage effect shows, or -1 for none (the original has one rule while the
// car runs, another once it is a wreck):
//   running:  &8A from 81 %, &8B from 91 %, &8C from 96 %, frame (clock/4 + 2/4/6) & 7;
//             &89 and &8D are never drawn;
//   wrecked:  &89..&8C all burn, frame (clock/4) & 7; &8D plays frames 0..6, six timer ticks
//             each, from the moment the car died, then is gone.
static int damage_fx_phase(const VehicleDamageFx& d, const VehiclePose& pose) {
    const uint32_t c4 = pose.fx_clock >> 2;
    if (pose.wrecked) {
        if (d.flags == 0x8D) {
            const uint32_t f = pose.wreck_clock / 6;
            return f <= 6 ? static_cast<int>(f) : -1;
        }
        return static_cast<int>(c4 & 7);
    }
    if (d.flags == 0x8A && pose.damage_percent >= 81) return static_cast<int>((c4 + 2) & 7);
    if (d.flags == 0x8B && pose.damage_percent >= 91) return static_cast<int>((c4 + 4) & 7);
    if (d.flags == 0x8C && pose.damage_percent >= 96) return static_cast<int>((c4 + 6) & 7);
    return -1;
}

std::vector<TerrainVertex> place_vehicle_effects(const VehicleModel& model, const VehiclePose& pose,
                                                 const float cam[3], const float right[3],
                                                 const float up[3], int map_w,
                                                 float render_scale) {
    std::vector<TerrainVertex> out;
    if (model.damage_fx.empty()) return out;
    // The body's rotation, the same formula as place_vehicle's.
    constexpr float kBamToRad = 6.283185307179586f / 65536.0f;
    const float sa = std::sin(pose.pitch * kBamToRad), ca = std::cos(pose.pitch * kBamToRad);
    const float sb = std::sin(pose.yaw * kBamToRad), cb = std::cos(pose.yaw * kBamToRad);
    const float sc = std::sin(pose.roll * kBamToRad), cc = std::cos(pose.roll * kBamToRad);
    const float rot[9] = {sb * sc * sa + cb * cc, cb * sc - sb * cc * sa, ca * sb,
                          -ca * sc,               ca * cc,                sa,
                          cb * sc * sa - sb * cc, -(cb * cc * sa + sb * sc), ca * cb};
    const float mirror_axis =
        static_cast<float>(map_w - 1) * static_cast<float>(kTerrainTexelWorldSize);
    const float pull = model.radius * render_scale;
    // Nearer the eye by `pull`, never past it.
    auto pulled = [&](TerrainVertex& o) {
        const float dx = cam[0] - o.x, dy = cam[1] - o.y, dz = cam[2] - o.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-3f) return;
        const float k = std::min(pull, len * 0.5f) / len;
        o.x += dx * k;
        o.y += dy * k;
        o.z += dz * k;
    };
    for (const VehicleDamageFx& d : model.damage_fx) {
        const int phase = damage_fx_phase(d, pose);
        if (phase < 0) continue;
        const size_t ph = static_cast<size_t>(phase);
        // The anchor rides the body; the effect itself is turned by minus the body's angles,
        // i.e. it stands upright in the world whatever the car is doing.
        const float ax = static_cast<float>(d.pos[0]), ay = static_cast<float>(d.pos[1]),
                    az = static_cast<float>(d.pos[2]);
        const float rx = rot[0] * ax + rot[1] * ay + rot[2] * az + pose.x;
        const float ry = rot[3] * ax + rot[4] * ay + rot[5] * az + pose.y;
        const float rz = rot[6] * ax + rot[7] * ay + rot[8] * az + pose.z;
        if (ph < d.by_phase.size())
            for (const TerrainVertex& v : d.by_phase[ph]) {
                TerrainVertex o = v;
                o.x = (mirror_axis - (v.x + rx)) * render_scale;
                o.y = -(-v.y + ry) * render_scale;
                o.z = (v.z + rz) * render_scale;
                pulled(o);
                out.push_back(o);
            }
        if (ph >= d.sprites_by_phase.size()) continue;
        for (const ParticleSprite& s : d.sprites_by_phase[ph]) {
            TerrainVertex centre{};
            centre.x = (mirror_axis - (s.x + rx)) * render_scale;
            centre.y = -(-s.y + ry) * render_scale;
            centre.z = (s.z + rz) * render_scale;
            pulled(centre);
            const float hw = s.half_w * render_scale, hh = s.half_h * render_scale;
            static constexpr float kSx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            static constexpr float kSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
            static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            static constexpr float kV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            TerrainVertex corner[4];
            for (int i = 0; i < 4; ++i) {
                TerrainVertex v{};
                v.x = centre.x + right[0] * kSx[i] * hw + up[0] * kSy[i] * hh;
                v.y = centre.y + right[1] * kSx[i] * hw + up[1] * kSy[i] * hh;
                v.z = centre.z + right[2] * kSx[i] * hw + up[2] * kSy[i] * hh;
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

VehiclePose pose_of(const VehicleSim& sim) {
    VehiclePose p;
    // Straight off the fixed-point position, NOT off world_x() - that one truncates to a whole
    // world unit, and a truncated position interpolates in visible steps.
    p.x = static_cast<float>(sim.px) / 512.0f;
    p.y = static_cast<float>(sim.py) / 512.0f;
    p.z = static_cast<float>(sim.pz) / 512.0f;
    p.pitch = static_cast<float>(sim.pitch_bam16());
    p.yaw = static_cast<float>(sim.yaw_bam16());
    p.roll = static_cast<float>(sim.roll_bam16());
    p.steer = static_cast<float>(sim.steer_axis);
    p.wheel_spin = static_cast<float>(sim.wheel_spin);
    p.rotor_spin = static_cast<float>(sim.rotor_spin);
    p.boost_active = sim.boost_active;
    p.flame_phase = sim.flame_phase;
    p.damage_percent = sim.damage_percent;
    p.wrecked = sim.damage < 0;
    p.fx_clock = static_cast<uint32_t>(sim.tick) * 2u;   // two timer ticks per physics tick
    p.wreck_clock = static_cast<uint32_t>(sim.wrecked_ticks) * 2u;
    return p;
}

VehiclePose lerp_pose(const VehiclePose& a, const VehiclePose& b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // A `recover` puts the vehicle back 500 units over its grid slot, which can be the far side
    // of the map. Nothing driven covers 4000 units in one 28 ms tick, so a jump that size is a
    // teleport and must not be drawn as a movement.
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz > 4000.0f * 4000.0f) return b;

    auto mix = [t](float u, float v) { return u + (v - u) * t; };
    // BAM16 wraps at 65536: a heading crossing due north steps 65535 -> 0, and a straight
    // interpolation would spin the body a full turn between two ticks.
    auto mix_angle = [t](float u, float v) {
        float d = std::fmod(v - u + 98304.0f, 65536.0f) - 32768.0f;
        return u + d * t;
    };

    VehiclePose p = b;
    p.x = mix(a.x, b.x);
    p.y = mix(a.y, b.y);
    p.z = mix(a.z, b.z);
    p.pitch = mix_angle(a.pitch, b.pitch);
    p.yaw = mix_angle(a.yaw, b.yaw);
    p.roll = mix_angle(a.roll, b.roll);
    p.steer = mix(a.steer, b.steer);
    p.wheel_spin = mix_angle(a.wheel_spin, b.wheel_spin);
    p.rotor_spin = mix_angle(a.rotor_spin, b.rotor_spin);
    return p;
}

void vehicle_rotation(float pitch, float yaw, float roll, float m[9]) {
    const float sa = std::sin(pitch), ca = std::cos(pitch);
    const float sb = std::sin(yaw), cb = std::cos(yaw);
    const float sc = std::sin(roll), cc = std::cos(roll);
    m[0] = sb * sc * sa + cb * cc;   m[1] = cb * sc - sb * cc * sa;    m[2] = ca * sb;
    m[3] = -ca * sc;                 m[4] = ca * cc;                   m[5] = sa;
    m[6] = cb * sc * sa - sb * cc;   m[7] = -(cb * cc * sa + sb * sc); m[8] = ca * cb;
}

std::vector<TerrainVertex> place_vehicle(const VehicleModel& model, const VehiclePose& pose,
                                         int map_w, float render_scale,
                                         const VehicleLighting* light) {
    std::vector<TerrainVertex> out;
    if (!model.valid()) return out;
    // Lit only in a race (the model has DEFAULT.CG) and when the caller hands the light over.
    const bool lighting = light && model.shade_cg && !model.shade_palette.empty();

    // The orientation matrix, in the GAME's axes (+Y down) - the same formula the decor uses,
    // and the same one the simulation rotates its contact box with. The body takes it from the
    // pose; every attached part takes it from its own three `wheel` angles.
    constexpr float kBamToRad = 6.283185307179586f / 65536.0f;
    const auto make_rot = vehicle_rotation;
    float rot[9];
    make_rot(pose.pitch * kBamToRad, pose.yaw * kBamToRad, pose.roll * kBamToRad, rot);
    static const float kNoRotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    const float mirror_axis =
        static_cast<float>(map_w - 1) * static_cast<float>(kTerrainTexelWorldSize);
    const float wx = pose.x;
    const float wy = pose.y;
    const float wz = pose.z;

    // local render space (+Y up) -> game local (+Y down) -> rotate -> translate -> mirror X.
    // `spin` turns the part about its OWN Z first: a wheel mesh is authored with its axle along
    // Z and stood up by the `wheel` line's 90-degree Y rotation, so that is the rolling axis.
    auto emit = [&](const TerrainVertex& v, const float part[9], float ox, float oy, float oz,
                    float spin_cos = 1.0f, float spin_sin = 0.0f,
                    const VertexShade* sh = nullptr) {
        // The roll, in the MESH's own frame - which is where a wheel's axle is, authored along
        // its local Z. It has to be applied here, BEFORE the part's own rotation: that rotation
        // carries the steering, so rolling after it would spin a steered wheel about an axis
        // that is no longer its axle, and the wheel would visibly cone about a third axis as it
        // turns. Which side the wheel is on is settled by the SIGN of the angle, in
        // place_vehicle, not by the order.
        const float gy0 = -v.y; // .3DW Y is inverted; game-space (+Y down) from here on
        const float sx = v.x * spin_cos - gy0 * spin_sin;
        const float sgy = v.x * spin_sin + gy0 * spin_cos;
        // Then the part's own orientation, and finally the body's.
        const float lx = part[0] * sx + part[1] * sgy + part[2] * v.z + ox;
        const float gy = part[3] * sx + part[4] * sgy + part[5] * v.z + oy; // offsets are
        const float lz = part[6] * sx + part[7] * sgy + part[8] * v.z + oz; // already game-space
        const float rx = rot[0] * lx + rot[1] * gy + rot[2] * lz;
        const float ry = rot[3] * lx + rot[4] * gy + rot[5] * lz;
        const float rz = rot[6] * lx + rot[7] * gy + rot[8] * lz;
        TerrainVertex o = v;
        o.x = (mirror_axis - (rx + wx)) * render_scale;
        o.y = -(ry + wy) * render_scale; // game +Y down -> render +Y up
        o.z = (rz + wz) * render_scale;
        // THE LIGHTING: the normal turned exactly like the
        // vertex, its cosine with the sun, `level = intensity x cos` (0 facing away), and the
        // face's index walked up DEFAULT.CG's row for that level, as the original does.
        // The colours are then interpolated across the face, as the original's fill does.
        if (lighting && sh && sh->idx >= 0) {
            const float snx = sh->nx * spin_cos - sh->ny * spin_sin;
            const float sny = sh->nx * spin_sin + sh->ny * spin_cos;
            const float px = part[0] * snx + part[1] * sny + part[2] * sh->nz;
            const float py = part[3] * snx + part[4] * sny + part[5] * sh->nz;
            const float pz = part[6] * snx + part[7] * sny + part[8] * sh->nz;
            const float nx = rot[0] * px + rot[1] * py + rot[2] * pz;
            const float ny = rot[3] * px + rot[4] * py + rot[5] * pz;
            const float nz = rot[6] * px + rot[7] * py + rot[8] * pz;
            const float c = nx * light->sun[0] + ny * light->sun[1] + nz * light->sun[2];
            int level = c > 0.0f ? static_cast<int>(c * static_cast<float>(light->intensity)) : 0;
            if (level > 255) level = 255;
            const uint8_t* row =
                shade_row(model.shade_cg->data(), model.shade_cg->size(), level);
            const int li = row ? row[sh->idx & 0xFF] : sh->idx;
            float r, g, b;
            face_rgb(li, model.shade_palette, model.shade_tinted ? &model.shade_tint : nullptr,
                     model.shade_colour, r, g, b);
            o.r = o.br = r;
            o.g = o.bg = g;
            o.b = o.bb = b;
        }
        out.push_back(o);
    };

    out.reserve(model.body.size() + model.parts.size() * 64);
    // A wreck is drawn as the vehicle's own wreck shape when it has one, as in the original.
    const std::vector<TerrainVertex>& body =
        pose.wrecked && !model.wreck_body.empty() ? model.wreck_body : model.body;
    const bool body_lit = &body == &model.body && model.body_shade.size() == body.size();
    for (size_t i = 0; i < body.size(); ++i)
        emit(body[i], kNoRotation, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
             body_lit ? &model.body_shade[i] : nullptr);

    // Steered parts yaw with the steering input; `steer_axis` is +/-256 at full lock and the
    // gain per part comes straight from the original (WheelAttach::steer_bam_per_unit).
    const float steer = pose.steer;
    const float spin = pose.wheel_spin * kBamToRad;
    const float spin_c = std::cos(spin), spin_s = std::sin(spin);
    // The powered parts turn on their own clock, about the same axis a wheel rolls on - their
    // own local Z. Nothing else is needed to make the UFO's two discs COUNTER-rotate: they are
    // one shape pitched +90 and -90, so a single turn in the mesh's frame comes out opposite in
    // the world. The property that had the two sides of a car rolling against each other is,
    // here, exactly what the authors were after.
    const float rspin = pose.rotor_spin * kBamToRad;
    const float rot_c = std::cos(rspin), rot_s = std::sin(rspin);
    for (size_t pi = 0; pi < model.parts.size(); ++pi) {
        const VehiclePart& p = model.parts[pi];
        float part_rot[9];
        if (pose.wrecked && pi < model.wreck_rot.size()) {
            // A wreck: the decorative pieces are gone - the driver's head, the
            // tracks, the spare wheel (`&FF` returns undrawn) - and every other part lies at the
            // random angles it was thrown to, still.
            if (p.flags == 0xFF) continue;
            const std::array<int32_t, 3>& r = model.wreck_rot[pi];
            make_rot(static_cast<float>(r[0]) * kBamToRad, static_cast<float>(r[1]) * kBamToRad,
                     static_cast<float>(r[2]) * kBamToRad, part_rot);
            for (const TerrainVertex& v : p.local)
                emit(v, part_rot, static_cast<float>(p.pos[0]), static_cast<float>(p.pos[1]),
                     static_cast<float>(p.pos[2]));
            continue;
        }
        const float base = static_cast<float>(p.rot_bam16[1]) * kBamToRad;
        const float part_roll = p.roll_steer_gain != 0
                                    ? steer * static_cast<float>(p.roll_steer_gain)
                                    : static_cast<float>(p.rot_bam16[2]);
        make_rot(static_cast<float>(p.rot_bam16[0]) * kBamToRad,
                 base + steer * static_cast<float>(p.steer_gain) * kBamToRad,
                 part_roll * kBamToRad, part_rot);
        // Every rolling wheel in the game is yawed a quarter turn, `&4000` on one side and
        // `&C000` on the other (25 each, checked over the 20 vehicles, and nothing else). So the
        // same roll in the mesh's frame sends one side forwards and the other backwards: the
        // side whose axle ends up pointing +X takes the opposite sign. Without this, half the
        // wheels on every vehicle turn as if the car were reversing - invisible on a plain round
        // tyre, obvious on the BUGGY's treaded ones, which is how the user caught it.
        const bool axle_reversed = std::sin(base) > 0.0f;
        const float sc = p.rolls ? spin_c : (p.powered ? rot_c : 1.0f);
        const float ss = p.rolls ? (axle_reversed ? -spin_s : spin_s)
                                 : (p.powered ? rot_s : 0.0f);
        const bool part_lit = p.shade.size() == p.local.size();
        for (size_t i = 0; i < p.local.size(); ++i)
            emit(p.local[i], part_rot, static_cast<float>(p.pos[0]), static_cast<float>(p.pos[1]),
                 static_cast<float>(p.pos[2]), sc, ss, part_lit ? &p.shade[i] : nullptr);
    }

    // The boost flame. The simulation keeps the two pieces of state the original does:
    // `boost_active` and the phase counter it cycles while the boost runs -
    // and that counter is the shape's variant byte, so it picks a frame of the flipbook baked
    // at load. The phase advances once per TICK, so the flame animates at one speed whatever
    // the display refreshes at.
    if (pose.boost_active && !pose.wrecked && !model.nitro_fx.empty()) {
        const size_t phase = static_cast<size_t>(pose.flame_phase) %
                             static_cast<size_t>(kFlamePhaseCount);
        for (const VehicleFx& p : model.nitro_fx) {
            if (phase >= p.by_phase.size()) continue;
            float fx_rot[9];
            make_rot(0.0f, static_cast<float>(p.yaw_bam16) * kBamToRad, 0.0f, fx_rot);
            for (const TerrainVertex& v : p.by_phase[phase])
                emit(v, fx_rot, static_cast<float>(p.pos[0]), static_cast<float>(p.pos[1]),
                     static_cast<float>(p.pos[2]));
        }
    }
    return out;
}

} // namespace bigred
