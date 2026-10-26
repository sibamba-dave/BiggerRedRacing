#include "level_scene.h"

#include "../formats/game_files.h"
#include "../formats/shading.h"
#include "../formats/texture.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace bigred {

bool g_verbose_logs = false;

std::string g_diag_shape_filter;
bool g_decor_lines = true;

namespace {

// Does an embedded texture reference already carry its `.TM` extension? Exactly one shape in the
// shipped data does - see the call site.
bool ends_with_tm(const std::string& s) {
    if (s.size() < 3) return false;
    const std::string tail = s.substr(s.size() - 3);
    return tail == ".TM" || tail == ".tm";
}

// Which MAP.SHL slots the "[diag]" traces apply to, derived from g_diag_shape_filter at load
// time. Replaces a hardcoded list of shape indices left over from an old tunnel investigation,
// which was useless for anything else and silently wrong once a level had different slots.
std::vector<bool> g_diag_slot;

// Scenery placeholder markers: MAP.DAT's position field is confirmed, but which real
// .FT/.3DW shape each record uses is NOT yet confirmed - rather than guess wrong shapes,
// render a small flat-shaded pyramid per instance, colour-coded by its MAP.GRP group, so the
// real placement density/distribution is visible and honest about what is/isn't known yet.
struct MarkerVertex { float x, y, z; };

std::vector<bigred::TerrainVertex> build_marker_mesh(const std::vector<bigred::SceneryInstance>& instances,
                                                       const uint8_t* mph_data, int map_w, int map_h,
                                                       float world_to_render_scale, float height_scale,
                                                       bool absolute_y = false) {
    // 8 fixed, visually distinct colours, cycled by group index.
    constexpr float kPalette[8][3] = {
        {0.90f, 0.30f, 0.30f}, {0.30f, 0.85f, 0.35f}, {0.35f, 0.55f, 0.95f}, {0.95f, 0.85f, 0.25f},
        {0.85f, 0.35f, 0.85f}, {0.30f, 0.85f, 0.85f}, {0.95f, 0.60f, 0.20f}, {0.90f, 0.90f, 0.90f},
    };
    // Deliberately oversized for this placeholder pass so 273 markers are actually visible
    // from the orbit camera's distance - not meant to represent real object scale.
    constexpr float kBaseHalf = 250.0f;
    constexpr float kHeight = 700.0f;

    std::vector<bigred::TerrainVertex> out;
    out.reserve(instances.size() * 18); // 6 triangles/pyramid * 3 verts

    for (const bigred::SceneryInstance& inst : instances) {
        int ci = (inst.group_index >= 0) ? (inst.group_index % 8) : 7;
        float r = kPalette[ci][0], g = kPalette[ci][1], b = kPalette[ci][2];

        // Ground_y source is an open question (session 22) - see place_scenery's matching
        // comment for the full writeup. `absolute_y` toggles between the two candidates live.
        float ground_y;
        if (absolute_y) {
            ground_y = -inst.y;
        } else {
            int gx = static_cast<int>(inst.x / bigred::kTerrainTexelWorldSize);
            int gz = static_cast<int>(inst.z / bigred::kTerrainTexelWorldSize);
            gx = std::max(0, std::min(map_w - 1, gx));
            gz = std::max(0, std::min(map_h - 1, gz));
            ground_y = static_cast<float>(mph_data[static_cast<size_t>(gz) * map_w + gx]) * height_scale;
        }

        // Pyramid: 4 base corners + apex, in raw world units, sitting on top of the terrain.
        MarkerVertex base[4] = {
            {inst.x - kBaseHalf, ground_y, inst.z - kBaseHalf},
            {inst.x + kBaseHalf, ground_y, inst.z - kBaseHalf},
            {inst.x + kBaseHalf, ground_y, inst.z + kBaseHalf},
            {inst.x - kBaseHalf, ground_y, inst.z + kBaseHalf},
        };
        MarkerVertex apex{inst.x, ground_y + kHeight, inst.z};

        auto vert = [&](MarkerVertex v) -> bigred::TerrainVertex {
            bigred::TerrainVertex tv{};
            // World X is mirrored, same convention as place_scenery/build_terrain_mesh - see
            // terrain.cpp's comment for the full writeup (session 22, user-confirmed). Uses
            // (map_w - 1), not map_w: build_terrain_mesh mirrors column gx to (width-1-gx), so
            // the mirror axis sits at (map_w-1)*texel, not map_w*texel (session 2026-09-14).
            tv.x = (static_cast<float>(map_w - 1) * bigred::kTerrainTexelWorldSize - v.x) * world_to_render_scale;
            tv.y = v.y * world_to_render_scale;
            tv.z = v.z * world_to_render_scale;
            tv.r = r; tv.g = g; tv.b = b;
            // Markers are our own debug pyramids, not game data: they carry no palette index,
            // so there is no ramp to walk. Same colour at both ends = immune to the lighting,
            // which is what you want from a diagnostic.
            tv.br = r; tv.bg = g; tv.bb = b;
            tv.has_texture = 0.0f;
            return tv;
        };

        for (int i = 0; i < 4; ++i) {
            out.push_back(vert(base[i]));
            out.push_back(vert(base[(i + 1) % 4]));
            out.push_back(vert(apex));
        }
    }
    return out;
}

// COPLANAR STACKING IN AN ARBITRARY PLANE.
//
// The original paints its faces into a depth-sorted queue, so "the next face covers the previous
// one" is free for it and it authors signs that way: a backing plate and the glyph drawn over it
// sit at exactly the same coordinates. We have a depth buffer, so they fight.
//
// The ground-decal pass further down handles the HORIZONTAL case (a helipad, a start line) and is
// left exactly as it was, validated. This one handles every other plane - CITY's motorway signs
// MSIGN1/MSIGN2 are a diamond plate with a border ring and an arrow glyph, all at local z = 28,
// which is what the user saw as "two textures fighting" on CITY4's direction signs. Measured
// before the fix: 24 of the shape's 36 emitted triangles overlapped a coplanar neighbour.
//
// `polys` are one shape's faces in local space, in FILE ORDER (which is paint order), with their
// tag=9 group. Returns, per face, the offset to add to each of its vertices: `layer` steps along
// the face's own normal, oriented away from the shape's centroid so a plate's glyph moves toward
// the viewer rather than into the plate.
struct CoplanarLift { float x = 0, y = 0, z = 0; };

std::vector<CoplanarLift> coplanar_lifts(
    const std::vector<std::vector<std::array<float, 3>>>& polys,
    const std::vector<int>& groups) {
    constexpr float kLiftStep = 2.0f;   // raw world units, same scale as the ground-decal lift
    constexpr int kMaxLayer = 8;
    const size_t n = polys.size();
    std::vector<CoplanarLift> out(n);
    if (n < 2) return out;

    // Newell normal, centroid, and axis-aligned box of each face.
    std::vector<std::array<float, 3>> nrm(n), ctr(n);
    std::vector<std::array<float, 6>> box(n);
    std::vector<bool> ok(n, false), horizontal(n, false);
    std::array<float, 3> shape_centre{0, 0, 0};
    size_t counted = 0;
    for (size_t i = 0; i < n; ++i) {
        const std::vector<std::array<float, 3>>& p = polys[i];
        if (p.size() < 3) continue;
        std::array<float, 3> nn{0, 0, 0}, cc{0, 0, 0};
        for (size_t k = 0; k < p.size(); ++k) {
            const std::array<float, 3>& a = p[k];
            const std::array<float, 3>& b = p[(k + 1) % p.size()];
            nn[0] += (a[1] - b[1]) * (a[2] + b[2]);
            nn[1] += (a[2] - b[2]) * (a[0] + b[0]);
            nn[2] += (a[0] - b[0]) * (a[1] + b[1]);
            cc[0] += a[0]; cc[1] += a[1]; cc[2] += a[2];
        }
        const float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len < 1e-6f) continue;
        for (int k = 0; k < 3; ++k) { nn[k] /= len; cc[k] /= static_cast<float>(p.size()); }
        nrm[i] = nn; ctr[i] = cc; ok[i] = true;
        horizontal[i] = std::fabs(nn[1]) > 0.99f;
        for (int k = 0; k < 3; ++k) { box[i][k] = box[i][k + 3] = p[0][static_cast<size_t>(k)]; }
        for (const std::array<float, 3>& v : p) {
            for (int k = 0; k < 3; ++k) {
                box[i][k] = std::min(box[i][k], v[static_cast<size_t>(k)]);
                box[i][k + 3] = std::max(box[i][k + 3], v[static_cast<size_t>(k)]);
            }
        }
        for (int k = 0; k < 3; ++k) shape_centre[k] += cc[k];
        ++counted;
    }
    if (counted == 0) return out;
    for (int k = 0; k < 3; ++k) shape_centre[k] /= static_cast<float>(counted);

    std::vector<int> layer(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!ok[i] || horizontal[i]) continue; // horizontal faces belong to the ground-decal pass
        for (size_t j = 0; j < i; ++j) {       // file order IS paint order: only earlier faces
            if (!ok[j] || horizontal[j]) continue;
            if (groups[j] != groups[i]) continue; // different groups are alternatives, not a stack
            const float d = nrm[i][0] * nrm[j][0] + nrm[i][1] * nrm[j][1] + nrm[i][2] * nrm[j][2];
            if (std::fabs(d) < 0.999f) continue;  // not parallel
            const float off = nrm[i][0] * (ctr[i][0] - ctr[j][0]) +
                              nrm[i][1] * (ctr[i][1] - ctr[j][1]) +
                              nrm[i][2] * (ctr[i][2] - ctr[j][2]);
            if (std::fabs(off) > 0.5f) continue;  // parallel but not the SAME plane
            // Strict overlap in area, on the two axes the plane actually spans. Faces that merely
            // touch edge to edge (a border ring against its plate) must stay level, or a visible
            // step appears between them.
            bool overlap = true;
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(nrm[i][k]) > 0.9f) continue; // the plane's own axis - skip
                if (!(box[i][k] < box[j][k + 3] && box[j][k] < box[i][k + 3])) { overlap = false; break; }
            }
            if (overlap) layer[i] = std::max(layer[i], layer[j] + 1);
        }
        layer[i] = std::min(layer[i], kMaxLayer);
    }
    for (size_t i = 0; i < n; ++i) {
        if (!ok[i] || layer[i] == 0) continue;
        // Push along the normal, away from the shape's middle, so a glyph drawn over a plate ends
        // up in front of it rather than buried inside.
        float sign = nrm[i][0] * (ctr[i][0] - shape_centre[0]) +
                     nrm[i][1] * (ctr[i][1] - shape_centre[1]) +
                     nrm[i][2] * (ctr[i][2] - shape_centre[2]);
        if (sign < 0.0f) sign = -1.0f; else sign = 1.0f;
        const float amount = static_cast<float>(layer[i]) * kLiftStep * sign;
        out[i] = CoplanarLift{nrm[i][0] * amount, nrm[i][1] * amount, nrm[i][2] * amount};
    }
    return out;
}

// Builds one shape's flat-coloured geometry (see LocalTri in the header).
//
// `groups` lists which tag=9 face groups to emit (0 is the base geometry every shape has; a
// positive value is one of the shape's extra groups - see threedw.h's frame_groups). Every shape
// is now built ONCE with all of its groups: which of those faces a given instance actually draws
// is decided per instance by replaying the display list's branches (faces_reached), and that is
// also how an animated shape changes pose.
std::vector<LocalTri> build_shape_local_mesh(const bigred::ThreeDW& model, const std::vector<bigred::RGB>& palette,
                                              const std::vector<bigred::RGB>& palette_bright,
                                              const std::vector<int>& groups = {0}) {
    std::vector<LocalTri> out;

    // A flat face sharing the exact same vertex SET as a textured_variant face is that face's
    // opposite-winding "back side" duplicate (CONFIRMED session 23 via ROADSIGN.3DW: its flat
    // face 0, colour 42, and its textured face 6 both use vertices {0,1,2,3} - the same thin
    // sign board, drawn once per side since nothing here backface-culls). Perfectly coplanar
    // duplicates z-fight, and since flat faces are emitted first, the depth test discards the
    // textured one every time regardless of what it draws - rendering the flat "back" colour
    // permanently, even head-on. Skip any flat face whose vertex set matches a textured one so
    // only the real (textured) side remains.
    // When the shape carries UV records, its colourless (tag=6) faces are textured quads drawn
    // by the texture path, NOT unresolvable logo slots - so they must not also be emitted here
    // as magenta placeholders, or the placeholder z-fights (and usually beats) the textured
    // version. This is what left every cloud sprite a big magenta quad. See
    // build_textured_faces_local for the confirmation of what tag=6 means on such shapes.
    const bool textures_its_logo_faces = !model.texture_variants.empty();
    // Same "is this face handled by the texture path instead" test build_textured_faces_local
    // itself uses for inclusion - kept in sync so the two functions agree on every face, not
    // just the ones with their own inline UV. CORRECTED session 2026-09-13: this used to check
    // ONLY `face.textured_variant`, which is deliberately false for a colourless face that
    // defers to the shape's variant list rather than inheriting a UV inline (see threedw.cpp's
    // inheritance-threshold fix) - ROADSIGN's face 6 (the arrow) is exactly that case, so its
    // flat "back" duplicate (face 0, colour 42) was never recognized as having a textured
    // counterpart and kept winning the depth tie, permanently hiding the arrow.
    auto is_texture_path_face = [&](const bigred::Face& face) {
        return face.textured_variant || (textures_its_logo_faces && !face.color.has_value());
    };
    std::vector<std::vector<int32_t>> textured_vertex_sets;
    for (const bigred::Face& face : model.faces) {
        if (!is_texture_path_face(face)) continue;
        std::vector<int32_t> sorted_idx = face.indices;
        std::sort(sorted_idx.begin(), sorted_idx.end());
        textured_vertex_sets.push_back(std::move(sorted_idx));
    }
    auto matches_textured_face = [&](const bigred::Face& face) {
        std::vector<int32_t> sorted_idx = face.indices;
        std::sort(sorted_idx.begin(), sorted_idx.end());
        for (const auto& s : textured_vertex_sets) {
            if (s == sorted_idx) return true;
        }
        return false;
    };

    // Which faces will actually be emitted below, in order - needed up front so alt-groups
    // (faces sharing an exact vertex set, e.g. CHEVRONS' two colours on one quad) can be
    // detected before triangulating. Mirrors the textured path's own alt-group detection
    // (main.cpp's `alt_keys`/`alt_groups` for `tex_info->faces`) but keyed by vertex INDEX set
    // rather than world position - equivalent within one shape, and simpler since these faces
    // never move between groups.
    std::vector<const bigred::Face*> emitted;
    for (const bigred::Face& face : model.faces) {
        if (face.indices.size() < 3) continue;
        if (std::find(groups.begin(), groups.end(), face.group) == groups.end()) continue;
        if (is_texture_path_face(face)) continue;
        if (!textured_vertex_sets.empty() && matches_textured_face(face)) continue;
        emitted.push_back(&face);
    }
    std::vector<std::vector<int32_t>> flat_keys;
    std::vector<std::vector<size_t>> flat_alt_groups; // indices into `emitted`
    for (size_t fi = 0; fi < emitted.size(); ++fi) {
        std::vector<int32_t> key = emitted[fi]->indices;
        std::sort(key.begin(), key.end());
        size_t g = 0;
        for (; g < flat_keys.size(); ++g) {
            if (flat_keys[g] == key) break;
        }
        if (g == flat_keys.size()) { flat_keys.push_back(std::move(key)); flat_alt_groups.emplace_back(); }
        flat_alt_groups[g].push_back(fi);
    }
    // Per emitted face: does ANOTHER face of the same vertex set run the opposite way round?
    // Without one the face is one-sided, and the original shows it from one side only.
    std::vector<bool> flat_two_sided(emitted.size(), false);
    {
        auto reversed = [](const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
            const size_t n = a.size();
            if (n != b.size() || n < 3) return false;
            for (size_t s = 0; s < n; ++s) {
                bool ok = true;
                for (size_t k = 0; k < n && ok; ++k) ok = a[k] == b[(s + n - k) % n];
                if (ok) return true;
            }
            return false;
        };
        for (const auto& members : flat_alt_groups)
            for (size_t a = 0; a < members.size(); ++a)
                for (size_t b = 0; b < members.size(); ++b)
                    if (a != b && reversed(emitted[members[a]]->indices, emitted[members[b]]->indices))
                        flat_two_sided[members[a]] = true;
    }
    // Per-emitted-face (alt_group, alt_index, alt_count), -1 for faces whose group has only 1
    // member (the overwhelmingly common case: no ambiguity, always draw).
    std::vector<std::array<int, 3>> alt_of(emitted.size(), {-1, 0, 1});
    for (int g = 0; g < static_cast<int>(flat_alt_groups.size()); ++g) {
        const auto& members = flat_alt_groups[static_cast<size_t>(g)];
        if (members.size() < 2) continue;
        for (int idx = 0; idx < static_cast<int>(members.size()); ++idx) {
            alt_of[members[static_cast<size_t>(idx)]] = {g, idx, static_cast<int>(members.size())};
        }
    }

    // A shape's non-zero tag=9 group can ALSO be two mirrored ALTERNATIVES concatenated back to
    // back, rather than one sub-part - CONFIRMED session 2026-09-13 via CHEVRONS.3DW (found from
    // an annotated user screenshot: a diamond that should read as a single directional chevron
    // was showing both a left- and a right-pointing arrow superimposed). Its one extra group (16
    // faces, all colour 38) is really two complete 8-face chevron decals - one pointing each way,
    // authored back to back in file order - meant to be chosen by the SAME MAP.DAT offset-19
    // that already picks the backing panel's colour (the exact-vertex-set alt-groups above).
    // Nothing in the tag=9 marker declares this split; it has to be inferred from the group's
    // own face list dividing cleanly into two congruent (equal vertex count, comparable
    // bounding-box size - NOT expected to be pixel-identical like a true front/back duplicate,
    // since these are independently-authored mirror art, not the same geometry re-wound) halves,
    // by FILE ORDER, so this generalizes to any other shape with the same authoring pattern
    // without hardcoding CHEVRONS' own face numbers.
    {
        std::vector<int> group_ids;
        std::vector<std::vector<size_t>> group_members; // positions into `emitted`, file order
        for (size_t fi = 0; fi < emitted.size(); ++fi) {
            int gid = emitted[fi]->group;
            if (gid <= 0) continue; // base geometry - the exact-vertex-set pass above covers it
            size_t gi = 0;
            for (; gi < group_ids.size(); ++gi) if (group_ids[gi] == gid) break;
            if (gi == group_ids.size()) { group_ids.push_back(gid); group_members.emplace_back(); }
            group_members[gi].push_back(fi);
        }
        auto half_bounds = [&](const std::vector<size_t>& half) {
            std::vector<int32_t> vset;
            for (size_t pos : half) {
                for (int32_t vi : emitted[pos]->indices) {
                    if (std::find(vset.begin(), vset.end(), vi) == vset.end()) vset.push_back(vi);
                }
            }
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int32_t vi : vset) {
                const bigred::Vec3i& v = model.vertices[static_cast<size_t>(vi)];
                float c[3] = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
                for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
            }
            return std::array<float, 4>{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], static_cast<float>(vset.size())};
        };
        int next_alt_group_id = static_cast<int>(flat_alt_groups.size());
        for (const auto& members : group_members) {
            if (members.size() < 2 || members.size() % 2 != 0) continue;
            size_t half = members.size() / 2;
            std::vector<size_t> first(members.begin(), members.begin() + static_cast<long>(half));
            std::vector<size_t> second(members.begin() + static_cast<long>(half), members.end());
            std::array<float, 4> b1 = half_bounds(first), b2 = half_bounds(second);
            if (b1[3] != b2[3]) continue; // different vertex counts - not a congruent pair
            bool size_matches = true;
            for (int k = 0; k < 3; ++k) {
                float denom = std::max(b1[static_cast<size_t>(k)], b2[static_cast<size_t>(k)]);
                if (denom > 1.0f && std::fabs(b1[static_cast<size_t>(k)] - b2[static_cast<size_t>(k)]) / denom > 0.25f) {
                    size_matches = false;
                    break;
                }
            }
            if (!size_matches) continue;
            int gid = next_alt_group_id++;
            for (size_t pos : first) alt_of[pos] = {gid, 0, 2};
            for (size_t pos : second) alt_of[pos] = {gid, 1, 2};
        }
    }

    // Non-horizontal coplanar separation, in the same local space the vertices are emitted in
    // (Y negated, halved) - see coplanar_lifts.
    std::vector<CoplanarLift> flat_lifts;
    {
        std::vector<std::vector<std::array<float, 3>>> polys;
        std::vector<int> poly_groups;
        polys.reserve(emitted.size());
        poly_groups.reserve(emitted.size());
        for (const bigred::Face* f : emitted) {
            std::vector<std::array<float, 3>> p;
            for (int32_t idx : f->indices) {
                const bigred::Vec3i& v = model.vertices[static_cast<size_t>(idx)];
                constexpr float sc = bigred::kModelVertexScale;
                p.push_back({static_cast<float>(v.x) * sc, -static_cast<float>(v.y) * sc,
                             static_cast<float>(v.z) * sc});
            }
            polys.push_back(std::move(p));
            poly_groups.push_back(f->group);
        }
        flat_lifts = coplanar_lifts(polys, poly_groups);
    }

    for (size_t fi = 0; fi < emitted.size(); ++fi) {
        const bigred::Face& face = *emitted[fi];
        const CoplanarLift lift = flat_lifts[fi];
        const auto [alt_group, alt_index, alt_count] = alt_of[fi];
        // `emitted` holds pointers into model.faces, so its index there is just the distance.
        const int face_index = model.faces.empty()
                                   ? -1 : static_cast<int>(&face - model.faces.data());

        float r, g, b, br, bg, bb;
        if (face.color.has_value() && *face.color >= 0 && static_cast<size_t>(*face.color) < palette.size()) {
            size_t ci = static_cast<size_t>(*face.color);
            const bigred::RGB& c = palette[ci];
            r = c.r / 255.0f; g = c.g / 255.0f; b = c.b / 255.0f;
            // The same index at the other end of its shading ramp. THIS is the fix for the
            // black tunnels: ITITUNN*'s faces are all index 31, which is the darkest rung of a
            // grey ramp, not the colour black - see shading.h.
            const bigred::RGB& cl = (ci < palette_bright.size()) ? palette_bright[ci] : c;
            br = cl.r / 255.0f; bg = cl.g / 255.0f; bb = cl.b / 255.0f;
        } else {
            r = 0.78f; g = 0.24f; b = 0.78f; // logo-slot/textured face placeholder, see threedw.h
            br = r; bg = g; bb = b;
        }
        auto vert = [&](int32_t idx) -> LocalTri {
            const bigred::Vec3i& v = model.vertices[static_cast<size_t>(idx)];
            // .3DW local Y is stored inverted (-Y = up) relative to world/view (+Y = up) - see
            // threedw.h. Confirmed visually: CHTREE's cone (wide skirt at local y=0, point at
            // local y=-2400) only points up out of the ground once negated.
            // kModelVertexScale halves the stored coordinates - see threedw.h.
            constexpr float s = bigred::kModelVertexScale;
            return LocalTri{static_cast<float>(v.x) * s, -static_cast<float>(v.y) * s,
                             static_cast<float>(v.z) * s, r, g, b, br, bg, bb,
                             alt_group, alt_index, alt_count, static_cast<int>(idx), face_index,
                             lift.x, lift.y, lift.z, !flat_two_sided[fi]};
        };
        for (size_t i = 1; i + 1 < face.indices.size(); ++i) {
            out.push_back(vert(face.indices[0]));
            out.push_back(vert(face.indices[i]));
            out.push_back(vert(face.indices[i + 1]));
        }
    }

    // THE LINES (tag=11). The original draws a line one pixel wide; a mesh has no such thing, so
    // each becomes a thin bar - two quads crossed along it, so it reads from any angle. Never
    // drawn before 2026-09-28: TELEPOLE's three cross-arms on DIRT2 were missing (user).
    // ⚠️ A line has no face number, and the per-instance replay gates by face: a line goes with
    // the first face after it in the stream (same group), or failing that the last one before -
    // the faces of the block it sits in. That is an approximation of the walk, not the walk.
    for (const bigred::LineSeg& ln : model.lines) {
        if (!g_decor_lines) break;
        if (std::find(groups.begin(), groups.end(), ln.group) == groups.end()) continue;
        if (static_cast<size_t>(ln.v0) >= model.vertices.size() ||
            static_cast<size_t>(ln.v1) >= model.vertices.size())
            continue;
        int face_index = -1;
        for (size_t fi = 0; fi < model.faces.size() && fi < model.face_stream_pos.size(); ++fi) {
            if (model.faces[fi].group != ln.group) continue;
            if (model.face_stream_pos[fi] > ln.stream_pos) { face_index = static_cast<int>(fi); break; }
            face_index = static_cast<int>(fi);
        }
        float r = 0.5f, g = 0.5f, b = 0.5f, br = r, bg = g, bb = b;
        if (ln.color >= 0 && static_cast<size_t>(ln.color) < palette.size()) {
            const bigred::RGB& c = palette[static_cast<size_t>(ln.color)];
            const bigred::RGB& cl =
                static_cast<size_t>(ln.color) < palette_bright.size() ? palette_bright[static_cast<size_t>(ln.color)] : c;
            r = c.r / 255.0f; g = c.g / 255.0f; b = c.b / 255.0f;
            br = cl.r / 255.0f; bg = cl.g / 255.0f; bb = cl.b / 255.0f;
        }
        constexpr float s = bigred::kModelVertexScale;
        const bigred::Vec3i& a = model.vertices[static_cast<size_t>(ln.v0)];
        const bigred::Vec3i& e = model.vertices[static_cast<size_t>(ln.v1)];
        const float p0[3] = {a.x * s, -a.y * s, a.z * s};
        const float p1[3] = {e.x * s, -e.y * s, e.z * s};
        float d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-4f) continue;
        for (float& c : d) c /= len;
        // Two axes across the line: one horizontal where the line is not vertical, then the
        // one square to both.
        float u[3] = {-d[2], 0.0f, d[0]};
        float ul = std::sqrt(u[0] * u[0] + u[2] * u[2]);
        if (ul < 1e-3f) { u[0] = 1.0f; u[1] = 0.0f; u[2] = 0.0f; ul = 1.0f; }
        for (float& c : u) c /= ul;
        const float w[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
        // Half the bar's thickness, in the same units as the vertices: 4 stored units, a fifth of
        // TELEPOLE's own 20-unit pole.
        constexpr float kHalf = 4.0f * s;
        auto corner = [&](const float* p, const float* axis, float sign, int vtx) {
            LocalTri t{p[0] + axis[0] * kHalf * sign, p[1] + axis[1] * kHalf * sign,
                       p[2] + axis[2] * kHalf * sign, r, g, b, br, bg, bb};
            t.vtx = vtx;
            t.face_index = face_index;
            return t;
        };
        const float* const axes[2] = {u, w};
        for (const float* axis : axes) {
            const LocalTri q0 = corner(p0, axis, -1.0f, ln.v0), q1 = corner(p0, axis, 1.0f, ln.v0);
            const LocalTri q2 = corner(p1, axis, 1.0f, ln.v1), q3 = corner(p1, axis, -1.0f, ln.v1);
            out.push_back(q0); out.push_back(q1); out.push_back(q2);
            out.push_back(q0); out.push_back(q2); out.push_back(q3);
        }
    }
    return out;
}

// A `textured_variant` face's own local-space vertex positions (Y already negated, same
// convention as LocalTri), in file order - the actual UV per vertex comes from whichever
// `UVVariant` a specific MAP.DAT instance selects (see scenery.h's offset-19 comment), so it
// isn't baked in here the way LocalTri's colour is.

// `groups` as in build_shape_local_mesh.
std::vector<TexturedFaceLocal> build_textured_faces_local(const bigred::ThreeDW& model,
                                                            const std::vector<int>& groups = {0}) {
    std::vector<TexturedFaceLocal> out;
    // A shape's textured geometry is every face with no flat colour: tag=5 (own inline UVs) AND
    // tag=6. tag=6 was long assumed to be "player-chosen vehicle logo only, unresolvable, draw
    // magenta" - but on decor that carries UV records (CLOUDS/CLOUDSLO and friends), the tag=6
    // face IS the textured quad and its UV comes from the shape's variant list, selected by
    // MAP.DAT offset-19. CONFIRMED session 2026-09-12 by decoding CLOUDS.3DW's tail: its two
    // faces are `{6, 4, face_num, v0..v3}` front/back records, followed by several `{5, X, 4,
    // 8 UVs, tex_id}` short-form crops - i.e. the geometry and the UVs are in separate records.
    // Without this, every cloud sprite on every track rendered as a big magenta quad.
    const bool has_variants = !model.texture_variants.empty();
    for (size_t face_index = 0; face_index < model.faces.size(); ++face_index) {
        const bigred::Face& face = model.faces[face_index];
        const bool untextured_logo_face = !face.color.has_value() && !face.textured_variant;
        if (!face.textured_variant && !(has_variants && untextured_logo_face)) continue;
        if (face.indices.size() < 3) continue;
        if (std::find(groups.begin(), groups.end(), face.group) == groups.end()) continue;
        TexturedFaceLocal tf;
        constexpr float s = bigred::kModelVertexScale; // see threedw.h
        for (int32_t idx : face.indices) {
            const bigred::Vec3i& v = model.vertices[static_cast<size_t>(idx)];
            tf.pos.push_back({static_cast<float>(v.x) * s, -static_cast<float>(v.y) * s,
                               static_cast<float>(v.z) * s});
            tf.vtx.push_back(static_cast<int>(idx));
        }
        if (face.uv.size() == face.indices.size()) tf.uv = face.uv;
        tf.group = face.group;
        tf.face_index = static_cast<int>(face_index);
        out.push_back(std::move(tf));
    }

    // STACKED COPLANAR DECALS. A shape can paint one flat face directly on top of another - the
    // starting grid is a 1024-wide slab (two quads meeting at x=0) with a 512-wide marking laid
    // over the middle of it, all at local y=0. The original renders back-to-front into a queue
    // with no depth buffer, so "later face wins" is free; we have a depth buffer, and exactly
    // coplanar geometry z-fights into the speckled mess the user reported on CITY4.
    //
    // So: give each face the number of EARLIER faces of this shape it is stacked on, and let
    // place_scenery lift it by that much. Deliberately narrow, because a blanket per-face lift
    // would visibly warp any shape with many faces:
    //   - only HORIZONTAL faces count (every vertex at the same local y). A decal lying on the
    //     ground is the case this exists for; a wall is not.
    //   - they must share that y exactly, and their XZ boxes must overlap in AREA. Two halves of
    //     one slab meeting edge to edge (the grid's own faces 0 and 1) do not overlap, so they
    //     stay on the same layer and no step appears between them.
    // Mark each face that is an earlier face's opposite-winding duplicate - see is_back_face.
    {
        auto reversed_of = [](const std::vector<int>& a, const std::vector<int>& b) {
            if (a.size() != b.size() || a.empty()) return false;
            std::vector<int> rev(a.rbegin(), a.rend());
            for (size_t r = 0; r < rev.size(); ++r) {
                bool same = true;
                for (size_t k = 0; k < rev.size(); ++k) {
                    if (rev[(r + k) % rev.size()] != b[k]) { same = false; break; }
                }
                if (same) return true;
            }
            return false;
        };
        // Newell normal and centroid, so the pairing also catches a back side whose vertex SET
        // differs from the front's - SNOW1's START is one quad on the back but two halves on the
        // front, so comparing index lists alone misses it.
        auto plane_of = [](const TexturedFaceLocal& f, std::array<float, 3>& n,
                            std::array<float, 3>& c) {
            n = {0.0f, 0.0f, 0.0f};
            c = {0.0f, 0.0f, 0.0f};
            const size_t m = f.pos.size();
            for (size_t k = 0; k < m; ++k) {
                const std::array<float, 3>& a = f.pos[k];
                const std::array<float, 3>& b = f.pos[(k + 1) % m];
                n[0] += (a[1] - b[1]) * (a[2] + b[2]);
                n[1] += (a[2] - b[2]) * (a[0] + b[0]);
                n[2] += (a[0] - b[0]) * (a[1] + b[1]);
                c[0] += a[0]; c[1] += a[1]; c[2] += a[2];
            }
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-6f) { n[0] /= len; n[1] /= len; n[2] /= len; }
            if (m) { c[0] /= m; c[1] /= m; c[2] /= m; }
            return len > 1e-6f;
        };
        auto boxes_touch = [](const TexturedFaceLocal& a, const TexturedFaceLocal& b) {
            float lo_a[3], hi_a[3], lo_b[3], hi_b[3];
            for (int k = 0; k < 3; ++k) {
                lo_a[k] = hi_a[k] = a.pos[0][static_cast<size_t>(k)];
                lo_b[k] = hi_b[k] = b.pos[0][static_cast<size_t>(k)];
            }
            for (const std::array<float, 3>& v : a.pos) {
                for (int k = 0; k < 3; ++k) {
                    lo_a[k] = std::min(lo_a[k], v[static_cast<size_t>(k)]);
                    hi_a[k] = std::max(hi_a[k], v[static_cast<size_t>(k)]);
                }
            }
            for (const std::array<float, 3>& v : b.pos) {
                for (int k = 0; k < 3; ++k) {
                    lo_b[k] = std::min(lo_b[k], v[static_cast<size_t>(k)]);
                    hi_b[k] = std::max(hi_b[k], v[static_cast<size_t>(k)]);
                }
            }
            for (int k = 0; k < 3; ++k) {
                if (hi_a[k] < lo_b[k] - 1.0f || hi_b[k] < lo_a[k] - 1.0f) return false;
            }
            return true;
        };

        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].pos.size() < 3) continue;
            std::array<float, 3> ni, ci;
            const bool have_i = plane_of(out[i], ni, ci);
            for (size_t j = 0; j < i; ++j) {
                if (out[j].is_back_face) continue; // a back side is not itself a front to pair with
                if (out[j].group != out[i].group) continue;
                if (reversed_of(out[j].vtx, out[i].vtx)) {
                    out[i].is_back_face = true;
                    out[j].has_back = true;
                    break;
                }
                if (!have_i || out[j].pos.size() < 3) continue;
                std::array<float, 3> nj, cj;
                if (!plane_of(out[j], nj, cj)) continue;
                const float dot = ni[0] * nj[0] + ni[1] * nj[1] + ni[2] * nj[2];
                if (dot > -0.99f) continue; // not facing opposite ways
                const float off = nj[0] * (ci[0] - cj[0]) + nj[1] * (ci[1] - cj[1]) +
                                   nj[2] * (ci[2] - cj[2]);
                if (std::fabs(off) > 1.0f) continue; // not in the same plane
                if (boxes_touch(out[i], out[j])) {
                    out[i].is_back_face = true;
                    out[j].has_back = true;
                    break;
                }
            }
        }
    }

    // Non-horizontal coplanar separation, shared with the flat path - see coplanar_lifts. Run
    // AFTER back-face marking so a dropped back side does not push its front up a layer.
    {
        std::vector<std::vector<std::array<float, 3>>> polys;
        std::vector<int> poly_groups;
        polys.reserve(out.size());
        poly_groups.reserve(out.size());
        for (const TexturedFaceLocal& f : out) {
            polys.push_back(f.is_back_face ? std::vector<std::array<float, 3>>{} : f.pos);
            poly_groups.push_back(f.group);
        }
        const std::vector<CoplanarLift> lifts = coplanar_lifts(polys, poly_groups);
        for (size_t i = 0; i < out.size(); ++i) {
            out[i].lx = lifts[i].x; out[i].ly = lifts[i].y; out[i].lz = lifts[i].z;
        }
    }

    auto flat_y = [](const TexturedFaceLocal& f, float& y_out) {
        y_out = f.pos[0][1];
        for (const std::array<float, 3>& v : f.pos) {
            if (std::fabs(v[1] - y_out) > 0.001f) return false;
        }
        return true;
    };
    auto same_vertex_set = [](const TexturedFaceLocal& a, const TexturedFaceLocal& b) {
        if (a.pos.size() != b.pos.size()) return false;
        std::vector<std::array<float, 3>> ka = a.pos, kb = b.pos;
        std::sort(ka.begin(), ka.end());
        std::sort(kb.begin(), kb.end());
        return ka == kb;
    };
    auto xz_box = [](const TexturedFaceLocal& f, float box[4]) {
        box[0] = box[2] = f.pos[0][0];
        box[1] = box[3] = f.pos[0][2];
        for (const std::array<float, 3>& v : f.pos) {
            box[0] = std::min(box[0], v[0]); box[2] = std::max(box[2], v[0]);
            box[1] = std::min(box[1], v[2]); box[3] = std::max(box[3], v[2]);
        }
    };
    for (size_t i = 0; i < out.size(); ++i) {
        float yi = 0.0f;
        if (out[i].pos.empty() || !flat_y(out[i], yi)) continue;
        float bi[4];
        xz_box(out[i], bi);
        for (size_t j = 0; j < i; ++j) {
            // Only faces of the SAME tag=9 group can be drawn together; two groups covering the
            // same ground are alternatives (select_textured_faces), so stacking them is meaningless.
            if (out[j].group != out[i].group) continue;
            float yj = 0.0f;
            if (out[j].pos.empty() || !flat_y(out[j], yj)) continue;
            if (std::fabs(yi - yj) > 0.001f) continue;
            // Faces with the identical vertex SET are ALTERNATIVES, not a stack - place_scenery
            // emits exactly one of them per instance. Counting them as layers would pile up a
            // lift for geometry that is never drawn together (the grid's three marking designs
            // would have pushed the chosen one three steps up instead of one).
            if (same_vertex_set(out[i], out[j])) continue;
            float bj[4];
            xz_box(out[j], bj);
            // Strict overlap: touching edges (max == min) is not stacking.
            const bool overlap = bi[0] < bj[2] && bj[0] < bi[2] && bi[1] < bj[3] && bj[1] < bi[3];
            if (overlap) out[i].decal_layer = std::max(out[i].decal_layer, out[j].decal_layer + 1);
        }
    }
    return out;
}

// Per-shape texture-variant data (see threedw.h's UVVariant / scenery.h's offset-19 comment):
// the shape's own `textured_variant` face(s) in local space, its list of selectable UV crops,
// and which layer of the shared decor texture array (see main()'s atlas loading) holds the
// `.TM` those crops index into. `layer < 0` means no matching `.TM` was found/loadable, in
// which case these faces just aren't emitted (no placeholder - cleaner than a magenta blob for
// a case we now understand well enough to just skip until the texture is available).

// Which textured faces of a shape an instance with this offset-19 actually draws.
//
//
// Three nested rules, all of them read off the data rather than assumed:
//
//  1. Faces on the IDENTICAL vertex set are alternatives on one quad. A face whose vertex loop
//     is the reverse (to a cyclic rotation) of the current design's is that design's BACK side,
//     not a new design - CONFIRMED on CHOPPER1's START.3DW, whose four faces on {0,1,2,3} run
//     [0,1,2,3] / [1,0,3,2] / [0,1,2,3] / [1,0,3,2]. Only one face per design is emitted: the
//     back is exactly coplanar and nothing backface-culls here, so drawing it too just z-fights.
//
//  2. Faces on DIFFERENT vertex sets within one tag=9 group are parts of the same design and are
//     drawn together - the grid slab is two quads meeting at x=0.
//
//  3. Two tag=9 GROUPS whose horizontal footprints cover the same ground are ALTERNATIVES of
//     each other, and offset-19 indexes their designs end to end. CONFIRMED on S_GRID.3DW by the
//     placement geometry: group 2 is a 1024-wide slab, group 3 is a 512-wide strip with three
//     designs (coarse checker / fine checker / diagonal hazard stripes, three differently-SIZED
//     crops of ROADSIGN.TM). Read as "slab + marking on top" the pieces overlap and z-fight;
//     read as four alternatives they tile the start straight EXACTLY edge to edge, on CITY4
//     ([53248,53760][53760,54784][54785,55297][55297,55809]) and on CHOPPER1
//     ([49153,50177][50177,50689][50689,51713]) alike. Groups that do NOT overlap are genuine
//     sub-parts and all of them are drawn - ROADSIGN's frame, arrow overlay and second frame.
//
// ⚠️ `crop_index_out`, when given, receives the index of the chosen design WITHIN ITS GROUP -
// which is what a face with no inline UV must use to pick from the shape's crop list, NOT the
// raw offset-19. The two S_GRID families make the difference visible: CITY/CHOPPER/RIG encode
// the three narrow designs as three FACES, DIRT/SNOW encode the same three as one face plus a
// three-entry CROP LIST. Same motifs (coarse checker / fine checker / diagonal stripes), same
// offset-19 values at the same grid positions - but indexing the crop list by the raw byte made
// DIRT/SNOW show the FINE checker where the other family shows the COARSE one, because offset-19
// 0 is spent on the wide slab and never reaches the crop list at all.
// CONFIRMED 2026-09-14 by decoding the original's own conditionals.
//
// The original does not index these designs at all: it BIT-TESTS the instance's variant byte.
// S_GRID.3DW carries three tag=9 conditionals, all with operator 0 (bitwise TEST):
//
//     if (byte19 & 1) -> marker C        else -> marker B
//   B: if (byte19 & 2) -> stripes        else -> the wide 1024 slab
//   C: if (byte19 & 2) -> fine checker   else -> the coarse 512 checker
//
// which gives 0 -> wide, 1 -> coarse, 2 -> stripes, 3 -> fine. Every jump target was verified
// to land exactly on a face record in the real file. That is why no design index was ever found
// in the data: there is none, the selection is by BITS.
//
// This function encodes that outcome as a permutation of our own file-order design list, which
// happens to reproduce it exactly for S_GRID. ⚠️ It is not the general mechanism - faces_reached
// is - and it survives only as the FALLBACK for shapes with no conditionals at all.
int narrow_design_order(int within_group_index) {
    switch (within_group_index) {
        case 1: return 2;
        case 2: return 1;
        default: return within_group_index;
    }
}

std::vector<bool> select_textured_faces(const ShapeTextureInfo& tex, int variant,
                                        int* crop_index_out = nullptr) {
    const std::vector<TexturedFaceLocal>& faces = tex.faces;
    std::vector<bool> draw(faces.size(), false);
    if (faces.empty()) return draw;

    auto opposite_winding = [](const std::vector<int>& a, const std::vector<int>& b) {
        if (a.size() != b.size() || a.empty()) return false;
        std::vector<int> rev(a.rbegin(), a.rend());
        for (size_t r = 0; r < rev.size(); ++r) { // any cyclic rotation of the reverse
            bool same = true;
            for (size_t k = 0; k < rev.size(); ++k) {
                if (rev[(r + k) % rev.size()] != b[k]) { same = false; break; }
            }
            if (same) return true;
        }
        return false;
    };
    auto vertex_key = [](const TexturedFaceLocal& f) {
        std::vector<std::array<float, 3>> k = f.pos;
        std::sort(k.begin(), k.end());
        return k;
    };
    auto horizontal = [](const TexturedFaceLocal& f, float& y) {
        if (f.pos.size() < 3) return false;
        y = f.pos[0][1];
        for (const std::array<float, 3>& v : f.pos) {
            if (std::fabs(v[1] - y) > 0.001f) return false;
        }
        return true;
    };

    // Groups, in order of first appearance.
    std::vector<int> group_ids;
    for (const TexturedFaceLocal& f : faces) {
        if (std::find(group_ids.begin(), group_ids.end(), f.group) == group_ids.end()) {
            group_ids.push_back(f.group);
        }
    }

    // Per group: its designs, each a list of face indices (one per alt-slot).
    std::vector<std::vector<std::vector<size_t>>> group_designs(group_ids.size());
    for (size_t gi = 0; gi < group_ids.size(); ++gi) {
        // slot = one quad; each slot holds its designs in file order
        std::vector<std::vector<std::array<float, 3>>> slot_keys;
        std::vector<std::vector<size_t>> slot_design_first; // first face of each design, per slot
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            if (faces[fi].group != group_ids[gi] || faces[fi].pos.size() < 3) continue;
            std::vector<std::array<float, 3>> key = vertex_key(faces[fi]);
            size_t sl = 0;
            for (; sl < slot_keys.size(); ++sl) {
                if (slot_keys[sl] == key) break;
            }
            if (sl == slot_keys.size()) { slot_keys.push_back(std::move(key)); slot_design_first.emplace_back(); }
            if (!slot_design_first[sl].empty() &&
                opposite_winding(faces[slot_design_first[sl].back()].vtx, faces[fi].vtx)) {
                continue; // the back side of the design already recorded - never emitted
            }
            slot_design_first[sl].push_back(fi);
        }
        size_t ndesigns = 0;
        for (const std::vector<size_t>& sd : slot_design_first) ndesigns = std::max(ndesigns, sd.size());
        group_designs[gi].resize(ndesigns);
        for (const std::vector<size_t>& sd : slot_design_first) {
            for (size_t d = 0; d < ndesigns; ++d) {
                // A slot with fewer designs than the group keeps showing its last one.
                group_designs[gi][d].push_back(sd[std::min(d, sd.size() - 1)]);
            }
        }
    }

    // Do any two groups cover the same ground horizontally?
    auto group_box = [&](size_t gi, float box[4], float& y) {
        bool any = false;
        for (const TexturedFaceLocal& f : faces) {
            float fy = 0.0f;
            if (f.group != group_ids[gi] || !horizontal(f, fy)) continue;
            for (const std::array<float, 3>& v : f.pos) {
                if (!any) { any = true; box[0] = box[2] = v[0]; box[1] = box[3] = v[2]; y = fy; }
                box[0] = std::min(box[0], v[0]); box[2] = std::max(box[2], v[0]);
                box[1] = std::min(box[1], v[2]); box[3] = std::max(box[3], v[2]);
            }
        }
        return any;
    };
    bool groups_are_alternatives = false;
    for (size_t a = 0; a < group_ids.size() && !groups_are_alternatives; ++a) {
        float ba[4], ya = 0.0f;
        if (!group_box(a, ba, ya)) continue;
        for (size_t b = a + 1; b < group_ids.size(); ++b) {
            float bb[4], yb = 0.0f;
            if (!group_box(b, bb, yb)) continue;
            if (std::fabs(ya - yb) > 8.0f) continue;
            if (ba[0] < bb[2] && bb[0] < ba[2] && ba[1] < bb[3] && bb[1] < ba[3]) {
                groups_are_alternatives = true;
                break;
            }
        }
    }

    if (groups_are_alternatives) {
        std::vector<const std::vector<size_t>*> all;
        for (const std::vector<std::vector<size_t>>& gd : group_designs) {
            for (const std::vector<size_t>& d : gd) all.push_back(&d);
        }
        if (all.empty()) return draw;
        // Which group offset-19 lands in, and how far into that group.
        const int raw = std::max(0, variant);
        size_t gi = 0, base = 0;
        while (gi + 1 < group_designs.size() &&
               static_cast<size_t>(raw) >= base + group_designs[gi].size()) {
            base += group_designs[gi].size();
            ++gi;
        }
        const int within = narrow_design_order(raw - static_cast<int>(base));
        if (crop_index_out) *crop_index_out = within;
        const std::vector<std::vector<size_t>>& gd = group_designs[gi];
        if (!gd.empty()) {
            size_t pick = std::min<size_t>(static_cast<size_t>(std::max(0, within)), gd.size() - 1);
            for (size_t fi : gd[pick]) draw[fi] = true;
        }
        return draw;
    }

    for (const std::vector<std::vector<size_t>>& gd : group_designs) {
        if (gd.empty()) continue;
        size_t pick = std::min<size_t>(static_cast<size_t>(std::max(0, variant)), gd.size() - 1);
        for (size_t fi : gd[pick]) draw[fi] = true;
    }
    return draw;
}

// CROSS-INSTANCE DECAL STACKING.
//
// build_textured_faces_local separates faces stacked WITHIN one shape. This does the same
// between INSTANCES, which turns out to matter more: the four starting-grid slabs of CITY4
// sit at z = 53504 / 54272 / 55041 / 55553 with a 1024-long footprint each, so every
// consecutive pair overlaps by about 256 units at exactly the same height, and so do the
// markings laid over them. The original never notices - it paints into a depth-sorted queue,
// so the later record simply covers the earlier one - while a depth buffer turns the overlap
// strip into the speckled mess the user reported.
//
// Same rule as within a shape, applied in MAP.DAT record order: an instance whose flat
// ground footprint overlaps an EARLIER one sits one step above it. The layer is capped so a
// long chain of overlapping decals cannot accumulate a visible lift.
//
// O(instances^2), and the answer never changes for a loaded level - so LevelScene works
// it out once and hands it to every pass. See place_scenery's `decal_layers_in`.
std::vector<int> compute_decal_layers(
    const std::vector<bigred::SceneryInstance>& instances,
    const std::vector<ShapeTextureInfo>& shape_tex_by_shl_index) {
    constexpr int kMaxDecalLayer = 8;
    std::vector<int> instance_decal_layer(instances.size(), 0);
    // The local XZ box of the horizontal textured faces an instance ACTUALLY DRAWS, and
    // their local y. It has to be per instance, not per shape: on S_GRID the chosen design
    // is either a 1024-wide slab or a 512-wide strip, and measuring the shape's widest
    // possible extent reported overlaps between pieces that in fact tile edge to edge.
    struct Footprint { bool valid = false; float x0 = 0, z0 = 0, x1 = 0, z1 = 0, y = 0; };
    auto footprint_of = [](const ShapeTextureInfo& tex, int variant) {
        Footprint fp;
        const std::vector<bool> drawn = select_textured_faces(tex, variant);
        for (size_t fi = 0; fi < tex.faces.size(); ++fi) {
            if (!drawn[fi]) continue;
            const TexturedFaceLocal& f = tex.faces[fi];
            if (f.pos.size() < 3) continue;
            bool flat = true;
            for (const std::array<float, 3>& v : f.pos) {
                if (std::fabs(v[1] - f.pos[0][1]) > 0.001f) { flat = false; break; }
            }
            if (!flat) continue;
            for (const std::array<float, 3>& v : f.pos) {
                if (!fp.valid) {
                    fp.valid = true;
                    fp.x0 = fp.x1 = v[0]; fp.z0 = fp.z1 = v[2]; fp.y = v[1];
                } else {
                    fp.x0 = std::min(fp.x0, v[0]); fp.x1 = std::max(fp.x1, v[0]);
                    fp.z0 = std::min(fp.z0, v[2]); fp.z1 = std::max(fp.z1, v[2]);
                }
            }
        }
        return fp;
    };
    // The same box in world space. Rotation is yaw-dominated for ground decals, so taking
    // the axis-aligned bounds of the four rotated corners is exact enough to decide overlap.
    struct WorldBox { bool valid = false; float x0 = 0, z0 = 0, x1 = 0, z1 = 0, y = 0; };
    std::vector<WorldBox> boxes(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        const bigred::SceneryInstance& inst = instances[i];
        if (inst.shape_index < 0 ||
            static_cast<size_t>(inst.shape_index) >= shape_tex_by_shl_index.size()) continue;
        const Footprint fp = footprint_of(shape_tex_by_shl_index[static_cast<size_t>(inst.shape_index)],
                                          inst.variant_index);
        if (!fp.valid) continue;
        float rot[9];
        inst.rotation_matrix(rot);
        WorldBox wb;
        const float cx[4] = {fp.x0, fp.x1, fp.x1, fp.x0};
        const float cz[4] = {fp.z0, fp.z0, fp.z1, fp.z1};
        for (int c = 0; c < 4; ++c) {
            // Same convention as to_world below: +Y down in the original's matrix, so the
            // local y is negated going in and coming out.
            const float ly = -fp.y;
            const float wx = rot[0] * cx[c] + rot[1] * ly + rot[2] * cz[c] + inst.x;
            const float wz = rot[6] * cx[c] + rot[7] * ly + rot[8] * cz[c] + inst.z;
            const float wy = -(rot[3] * cx[c] + rot[4] * ly + rot[5] * cz[c]) + (-inst.y);
            if (c == 0) {
                wb.valid = true; wb.x0 = wb.x1 = wx; wb.z0 = wb.z1 = wz; wb.y = wy;
            } else {
                wb.x0 = std::min(wb.x0, wx); wb.x1 = std::max(wb.x1, wx);
                wb.z0 = std::min(wb.z0, wz); wb.z1 = std::max(wb.z1, wz);
            }
        }
        boxes[i] = wb;
    }
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!boxes[i].valid) continue;
        for (size_t j = 0; j < i; ++j) {
            if (!boxes[j].valid) continue;
            // Only decals at the same height can fight; a pad on a bridge deck above another
            // on the ground is genuinely separated and must not be lifted.
            if (std::fabs(boxes[i].y - boxes[j].y) > 8.0f) continue;
            const bool overlap = boxes[i].x0 < boxes[j].x1 && boxes[j].x0 < boxes[i].x1 &&
                                 boxes[i].z0 < boxes[j].z1 && boxes[j].z0 < boxes[i].z1;
            if (overlap) {
                instance_decal_layer[i] =
                    std::max(instance_decal_layer[i], instance_decal_layer[j] + 1);
            }
        }
        instance_decal_layer[i] = std::min(instance_decal_layer[i], kMaxDecalLayer);
    }
    return instance_decal_layer;
}

// Places real decor shapes at each MAP.DAT instance's confirmed (x,z) position, grounded on
// the real heightmap (same reasoning as build_marker_mesh - MAP.DAT's own Y didn't place
// things visibly). Shape choice is now CONFIRMED - `shape_index` (raw record byte 0) directly
// indexes the level's MAP.SHL-ordered shape list, matching the original's exact resolution
// formula. Falls back to a coloured placeholder marker
// only for an instance with an out-of-range index or an unloadable shape.
std::vector<bigred::TerrainVertex> place_scenery(
    const std::vector<bigred::SceneryInstance>& instances,
    const std::vector<std::vector<LocalTri>>& shapes_by_shl_index,
    const std::vector<ShapeTextureInfo>& shape_tex_by_shl_index,
    const std::vector<std::vector<bigred::Vec3i>>& shape_probes_by_shl_index,
    const std::vector<LevelScene::ShapeBranches>& shape_branches_by_shl_index,
    const uint8_t* mph_data, int map_w, int map_h, float world_to_render_scale,
    float height_scale, bool absolute_y = false,
    const std::vector<bool>& shape_is_empty_by_shl_index = {},
    bool draw_markers = true,
    // Per-instance overrides for this pass (parallel to `instances`), or empty for "every
    // instance, with its own MAP.DAT variant byte and all of its faces" - see InstancePass.
    const std::vector<InstancePass>& passes = {},
    // When set, the pass also collects the level's billboards (see SpritePlacement). Only the
    // STATIC pass does this: a sprite's quad is rebuilt per frame, not baked per animation step,
    // so it has to know its visibility across the whole cycle at once.
    std::vector<SpritePlacement>* sprites_out = nullptr,
    // Cross-instance decal layers, precomputed. The prepass that derives them is O(instances^2)
    // and its answer never changes, so LevelScene works it out once and every pass borrows it.
    // Null recomputes it, which is what a caller outside LevelScene gets.
    const std::vector<int>* decal_layers_in = nullptr,
    // When set, receives {first vertex, count} per instance for what THIS pass emitted - zero
    // count for an instance it skipped. That is how a destructible's two states end up as two
    // draw ranges of one buffer instead of two buffers. See LevelScene::WreckPiece.
    std::vector<std::pair<int, int>>* ranges_out = nullptr) {

    std::vector<bigred::TerrainVertex> out;
    std::vector<bigred::SceneryInstance> unshaped;

    // THE ORIGINAL'S OWN SELECTION. The compiled display list carries conditional branches on the
    // instance's variant byte, so which faces an instance draws is decided by replaying that
    // control flow - see faces_reached. This supersedes the
    // heuristics below (winding pairs, coplanar overlap, design permutations), which only ever
    // approximated its results.
    //
    // Cached per (slot, variant): a level reuses a handful of variant values across hundreds of
    // instances.
    std::map<std::tuple<int, int, bool>, bigred::DisplayListReplay> reach_cache;
    auto replay_for = [&](int slot, int variant, bool prologue) -> const bigred::DisplayListReplay* {
        if (slot < 0 || static_cast<size_t>(slot) >= shape_branches_by_shl_index.size()) return nullptr;
        const LevelScene::ShapeBranches& sb = shape_branches_by_shl_index[static_cast<size_t>(slot)];
        if (sb.branches.empty() || sb.face_count == 0) return nullptr;
        auto key = std::make_tuple(slot, variant, prologue);
        auto it = reach_cache.find(key);
        if (it == reach_cache.end()) {
            it = reach_cache.emplace(key, bigred::replay_display_list(
                                              sb.branches, sb.face_stream_pos, sb.crop_stream_pos,
                                              sb.face_count, variant, {}, prologue)).first;
        }
        return &it->second;
    };

    // Cross-instance decal stacking - see compute_decal_layers. Borrowed when the caller has
    // it already, which is the normal case inside LevelScene.
    const bool have_layers = decal_layers_in && decal_layers_in->size() == instances.size();
    const std::vector<int> own_layers =
        have_layers ? std::vector<int>()
                    : compute_decal_layers(instances, shape_tex_by_shl_index);
    const std::vector<int>& instance_decal_layer = have_layers ? *decal_layers_in : own_layers;

    if (ranges_out) ranges_out->assign(instances.size(), {0, 0});
    size_t inst_index = 0;
    for (const bigred::SceneryInstance& inst : instances) {
        const size_t this_index = inst_index;
        const int inst_layer = (inst_index < instance_decal_layer.size())
                                   ? instance_decal_layer[inst_index] : 0;
        ++inst_index;
        // Everything this instance emits lands contiguously, so one {first, count} describes it.
        const size_t emit_first = out.size();
        struct RangeCloser {
            std::vector<std::pair<int, int>>* ranges; size_t index; size_t first;
            const std::vector<bigred::TerrainVertex>* out;
            ~RangeCloser() {
                if (!ranges || index >= ranges->size()) return;
                (*ranges)[index] = {static_cast<int>(first),
                                    static_cast<int>(out->size() - first)};
            }
        } closer{ranges_out, this_index, emit_first, &out};
        // The distance lighting is for the TUNNELS only (user, 2026-09-27): whatever this
        // instance emits is marked lit when its category is a tunnel's - SimObject::tunnel's test.
        struct TunnelLit {
            std::vector<bigred::TerrainVertex>* out; size_t first; bool on;
            ~TunnelLit() {
                if (on)
                    for (size_t k = first; k < out->size(); ++k) (*out)[k].lit = 1.0f;
            }
        } tunnel_lit{&out, emit_first,
                     bigred::category_is_tunnel(
                         bigred::collision_category_index(inst.collision_category_byte))};
        // What this pass wants from this instance. An animated instance is drawn TWICE across
        // the two kinds of pass - once for the faces its whole cycle shares, once for the faces
        // of the step currently showing - with `skip_faces` keeping the two halves disjoint.
        const InstancePass* ip = this_index < passes.size() ? &passes[this_index] : nullptr;
        if (ip && !ip->draw) continue;
        const int variant = ip ? static_cast<int>(ip->variant) : static_cast<int>(inst.variant_index);
        // A two-body shape is drawn damaged by entering its display list AT the prologue, whose
        // unconditional jump goes straight to that body - see replay_display_list.
        const bool prologue = ip && ip->enter_at_prologue;
        // True when the OTHER pass owns this face, so this one must leave it out.
        auto face_owned_elsewhere = [&](int face_index) {
            return ip && !ip->skip_faces.empty() && face_index >= 0 &&
                   static_cast<size_t>(face_index) < ip->skip_faces.size() &&
                   ip->skip_faces[static_cast<size_t>(face_index)];
        };
        const std::vector<LocalTri>* shape = nullptr;
        if (inst.shape_index >= 0 && static_cast<size_t>(inst.shape_index) < shapes_by_shl_index.size()) {
            const auto& candidate = shapes_by_shl_index[static_cast<size_t>(inst.shape_index)];
            if (!candidate.empty()) shape = &candidate;
        }
        const ShapeTextureInfo* tex_info = nullptr;
        if (inst.shape_index >= 0 && static_cast<size_t>(inst.shape_index) < shape_tex_by_shl_index.size()) {
            const auto& candidate = shape_tex_by_shl_index[static_cast<size_t>(inst.shape_index)];
            if (!candidate.faces.empty() && candidate.layer >= 0) tex_info = &candidate;
        }
        if (!shape && !tex_info) {
            // A shape whose `.3DW` parsed fine but genuinely declares no faces is INVISIBLE by
            // design, not unknown - don't fall back to a debug marker for it. `CHECKP.3DW` is
            // exactly this: h2 = 0 with 8 vertices, i.e. a bare trigger volume, which also
            // matches the original, where its remapped category hits no `case` in the
            // per-frame decor updater (it is never sent to any render path). CHOPPER1 has 14 of them, so
            // this is where the stray magenta pyramids/cones dotted around the track came from.
            // Markers are still drawn for shapes we actually failed to load, where a visible
            // "something is here and we don't know what" is the honest thing to show.
            bool known_empty = inst.shape_index >= 0 &&
                                static_cast<size_t>(inst.shape_index) < shape_is_empty_by_shl_index.size() &&
                                shape_is_empty_by_shl_index[static_cast<size_t>(inst.shape_index)];
            if (!known_empty && draw_markers) unshaped.push_back(inst);
            continue;
        }

        // Decor sits at MAP.DAT's own fixed (x,-y,z) - CONFIRMED by live A/B test against real
        // gameplay footage - NOT ground-snapped onto MAP.MPH at render time. `absolute_y`
        // defaults true; the MAP.MPH branch stays available (toggle with 'Y') for comparison.
        //
        // No per-category special cases. A checkpoint/bridge-relative height override was tried
        // for BALLOON and friends and was itself the bug: the real cause was the terrain height
        // scale (28 instead of 16). Every decor category is positioned by its own absolute
        // MAP.DAT (x,y,z), full stop.
        float ground_y;
        if (absolute_y) {
            ground_y = -inst.y;
        } else {
            int gx = std::max(0, std::min(map_w - 1, static_cast<int>(inst.x / bigred::kTerrainTexelWorldSize)));
            int gz = std::max(0, std::min(map_h - 1, static_cast<int>(inst.z / bigred::kTerrainTexelWorldSize)));
            ground_y = static_cast<float>(mph_data[static_cast<size_t>(gz) * map_w + gx]) * height_scale;
        }

        bool dbg = inst.shape_index >= 0 &&
                    static_cast<size_t>(inst.shape_index) < g_diag_slot.size() &&
                    g_diag_slot[static_cast<size_t>(inst.shape_index)];
        if (dbg) {
            std::fprintf(stderr, "[diag] instance shape_index=%d pos=(%.0f,%.0f,%.0f) angles=(%d,%d,%d) ground_y=%.2f decal_layer=%d\n",
                         inst.shape_index, inst.x, inst.y, inst.z, inst.angle_x, inst.angle_y, inst.angle_z, ground_y, inst_layer);
        }

        const bigred::DisplayListReplay* replay = replay_for(inst.shape_index, variant, prologue);
        const std::vector<bool>* reached = replay ? &replay->drawn : nullptr;

        float rot[9];
        inst.rotation_matrix(rot);
        if (dbg) {
            std::fprintf(stderr, "[diag]   rot = [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]\n",
                         rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8]);
        }

        // Shared world-space transform for both flat-coloured and textured decor geometry -
        // rotate the local position then translate, mirroring world X the same way terrain
        // does (session 22 - see build_terrain_mesh's comment).
        //
        // `rot` is ported from the original and expects the GAME's axes,
        // where +Y points DOWN, while `ly` arrives in RENDER space (+Y up) because the mesh
        // builders already applied the .3DW negation. Hence the flip before and after. For a
        // yaw-only instance rot[1]/rot[3]/rot[5] are all 0 and the bug cancels exactly, which is
        // why only STEPS (CHOPPER1's only shape with a non-zero pitch) ever looked wrong.
        //
        // FIXED session 2026-09-14: this used `map_w` here, one full texel (512 world units) off
        // from build_terrain_mesh's mirror axis at (map_w - 1) * texel (terrain mirrors column gx
        // to width-1-gx, not width-gx). Every decor instance was therefore offset by exactly one
        // whole grid cell along mirrored X - the "tunnels/signs shifted by one cell, same
        // direction everywhere" defect reported 2026-09-14 (textures were unaffected because
        // build_terrain_mesh already used the correct axis).
        auto to_world = [&](float lx, float ly, float lz) -> std::array<float, 3> {
            float gy = -ly; // render space (+Y up) -> game space (+Y down)
            float rx = rot[0] * lx + rot[1] * gy + rot[2] * lz;
            float ry = -(rot[3] * lx + rot[4] * gy + rot[5] * lz); // and back to +Y up
            float rz = rot[6] * lx + rot[7] * gy + rot[8] * lz;
            float wx = (static_cast<float>(map_w - 1) * bigred::kTerrainTexelWorldSize - (rx + inst.x)) *
                       world_to_render_scale;
            float wy = (ry + ground_y) * world_to_render_scale;
            float wz = (rz + inst.z) * world_to_render_scale;
            return {wx, wy, wz};
        };

        // GROUND CONFORMING. A shape drawn through the original's type-3 or type-8 queue path
        // is not rigid: a listed subset of its vertices has its Y rewritten from a heightmap
        // probe taken under that vertex. `conform_y[i]` is vertex i's replacement LOCAL y, or
        // NaN when it keeps its own.
        //
        // The gate is the MAP.DAT collision category: raw 244-254 covers queue type 3 (FENCE,
        // BILBORD*, SUPP, HUGPOSTS, WALL2, ST_POSTS, RAMP...) and, at 250, type 8 (the tunnel
        // mouths ITITUNN1/SNOTUNN1/HAWTUNN1/MEXTUNN1/ENT2). Every other category is rigid -
        // notably 131, the tunnel TUBES, which sit next to the mouths but must not conform.
        //
        // 242/243 are QUEUE TYPE 5, and type 5 conforms too - CONFIRMED 2026-09-14, closing the
        // "type 5: not yet identified" gap in the spec. The raw byte goes through the 37-entry
        // category table to indices 35/36, the per-frame decor updater sends those to a routine
        // that queues them as type 5, and at flush the original's type-5 case makes
        // the same conforming call, with the same sign
        // convention, as type 3. Found by chasing an object the user reported floating in the
        // air on SNOW1: it is SNOW1 alone that uses 243, for the 10 STACK2 bridge piers and its
        // 139 BILBORD1/2/3 - shapes that carry 244-254 (and do conform) on every other level.
        // 242 is never used by any shipped level.
        std::vector<float> conform_y;
        const uint8_t cat = inst.collision_category_byte;
        const bool conforms = (cat >= 244 && cat <= 254) || cat == 242 || cat == 243;
        if (conforms && static_cast<size_t>(inst.shape_index) < shape_probes_by_shl_index.size()) {
            const std::vector<bigred::Vec3i>& probes =
                shape_probes_by_shl_index[static_cast<size_t>(inst.shape_index)];
            conform_y.assign(probes.size(), std::numeric_limits<float>::quiet_NaN());
            for (size_t i = 0; i < probes.size(); ++i) {
                constexpr float s = bigred::kModelVertexScale;
                // The probe's own local position, rotated and translated like any other vertex.
                // Its world X/Z is where we sample; its local Y is an offset ABOVE the ground
                // the vertex keeps (0 for every probe seen so far, but the original adds it).
                float plx = static_cast<float>(probes[i].x) * s;
                float ply = -static_cast<float>(probes[i].y) * s;
                float plz = static_cast<float>(probes[i].z) * s;
                float prx = rot[0] * plx + rot[1] * (-ply) + rot[2] * plz;
                float prz = rot[6] * plx + rot[7] * (-ply) + rot[8] * plz;
                // Terrain grid coordinate. The X mirror cancels out: terrain puts column gx at
                // ((map_w-1) - gx) * texel and decor puts world X at (map_w-1) * texel - x, so
                // gx is simply x / texel. Getting this wrong sends every probe to the far side
                // of the map, onto plausible-looking but wrong ground.
                float fgx = (prx + inst.x) / bigred::kTerrainTexelWorldSize;
                float fgz = (prz + inst.z) / bigred::kTerrainTexelWorldSize;
                int gx0 = static_cast<int>(std::floor(fgx)), gz0 = static_cast<int>(std::floor(fgz));
                float tx = fgx - static_cast<float>(gx0), tz = fgz - static_cast<float>(gz0);
                auto at = [&](int a, int b) {
                    a = std::max(0, std::min(map_w - 1, a));
                    b = std::max(0, std::min(map_h - 1, b));
                    return static_cast<float>(mph_data[static_cast<size_t>(b) * map_w + a]) * height_scale;
                };
                float h = at(gx0, gz0) * (1 - tx) * (1 - tz) + at(gx0 + 1, gz0) * tx * (1 - tz) +
                          at(gx0, gz0 + 1) * (1 - tx) * tz + at(gx0 + 1, gz0 + 1) * tx * tz;
                // World height of the conformed vertex, brought back into the shape's local
                // space (whose origin sits at the instance's own ground_y).
                conform_y[i] = (h + ply) - ground_y;
            }
        }
        auto local_y = [&](int vtx, float fallback) {
            if (vtx >= 0 && static_cast<size_t>(vtx) < conform_y.size() && !std::isnan(conform_y[static_cast<size_t>(vtx)])) {
                return conform_y[static_cast<size_t>(vtx)];
            }
            return fallback;
        };

        // BILLBOARDS. Collected here because this is where the instance's world transform lives.
        // Visibility is per STEP of the instance's animation slot, since a light blinks: on SPIRE
        // the replay reaches both sprites on variants 1/2/3/5/6/7 and neither on 0/4.
        if (sprites_out && inst.shape_index >= 0 &&
            static_cast<size_t>(inst.shape_index) < shape_branches_by_shl_index.size()) {
            const LevelScene::ShapeBranches& sb =
                shape_branches_by_shl_index[static_cast<size_t>(inst.shape_index)];
            const ShapeTextureInfo* stex =
                (static_cast<size_t>(inst.shape_index) < shape_tex_by_shl_index.size())
                    ? &shape_tex_by_shl_index[static_cast<size_t>(inst.shape_index)] : nullptr;
            if (!sb.sprites.empty() && stex && stex->layer >= 0) {
                std::vector<int32_t> spos;
                spos.reserve(sb.sprites.size());
                for (const ShapeSprite& sp : sb.sprites) spos.push_back(sp.stream_pos);
                const std::vector<uint8_t> steps =
                    (ip && !ip->step_variants.empty()) ? ip->step_variants
                                                       : std::vector<uint8_t>{static_cast<uint8_t>(variant)};
                std::vector<bigred::DisplayListReplay> seen(steps.size());
                for (size_t k = 0; k < steps.size(); ++k) {
                    const bool kp = (ip && k < ip->step_prologue.size()) ? ip->step_prologue[k] != 0
                                                                        : prologue;
                    seen[k] = bigred::replay_display_list(sb.branches, sb.face_stream_pos,
                                                          sb.crop_stream_pos, sb.face_count,
                                                          steps[k], spos, kp);
                }
                for (size_t si = 0; si < sb.sprites.size(); ++si) {
                    SpritePlacement pl;
                    pl.frames.resize(steps.size());
                    bool ever = false;
                    for (size_t k = 0; k < steps.size(); ++k) {
                        if (si >= seen[k].sprite_drawn.size() || !seen[k].sprite_drawn[si]) continue;
                        // The crop the REPLAY had current here, not whichever came last in file
                        // order - that is what makes one sprite record cycle through frames.
                        const int c = seen[k].sprite_crop[si];
                        if (c < 0 || static_cast<size_t>(c) >= stex->variants.size()) continue;
                        const bigred::UVVariant& uv = stex->variants[static_cast<size_t>(c)];
                        pl.frames[k] = SpritePlacement::Frame{
                            true, uv.u0 / bigred::kTmWidth, uv.v0 / bigred::kTmHeight,
                            uv.u1 / bigred::kTmWidth, uv.v1 / bigred::kTmHeight,
                            stex->layer_for_variant(c)};
                        ever = true;
                    }
                    // Never reached on any step: this record belongs to a branch the instance does
                    // not take - on most shapes, the DESTROYED state behind variant bit 0x40.
                    if (!ever) continue;
                    const ShapeSprite& sp = sb.sprites[si];
                    const std::array<float, 3> w = to_world(sp.x, local_y(sp.vtx, sp.y), sp.z);
                    pl.x = w[0]; pl.y = w[1]; pl.z = w[2];
                    pl.half_w = sp.half_w * world_to_render_scale;
                    pl.half_h = sp.half_h * world_to_render_scale;
                    pl.layer = static_cast<float>(stex->layer);
                    pl.anim_slot = ip ? ip->anim_slot : -1;
                    pl.wreck = ip ? ip->wreck_index : -1;
                    sprites_out->push_back(std::move(pl));
                }
            }
        }

        // ⚠️ A FLAT RIGID SHAPE LYING ON THE GROUND IS DRAPED OVER IT. The original paints decor
        // over the terrain in depth order, with no depth buffer: a flat arrow placed a little
        // under a bump of the ground (MOON3's ARROW, rigid - it goes down the original's
        // general draw path) still shows whole. A depth buffer hides every part of it under
        // the bump, and the polygon offset only saves it from afar - "it sinks into the ground
        // as you come closer" (user, 2026-09-28). So its triangles are cut to at most
        // kDrapeStep and every point the ground covers, by up to kDrapeBand, is raised
        // kDrapeLift above it. A shape buried deeper than that, or standing clear, is untouched.
        bool drape = false;
        if (shape && !shape->empty() && !conforms) {
            drape = true;
            const float y0 = shape->front().y + shape->front().ly;
            for (const LocalTri& t : *shape)
                if (std::fabs(t.y + t.ly - y0) > 0.5f) { drape = false; break; }
        }
        // The exact terrain height under raw world (x, z): the renderer's own split of the
        // cell, main diagonal (build_terrain_mesh).
        auto terrain_at = [&](float x, float z) {
            const float fgx = x / bigred::kTerrainTexelWorldSize, fgz = z / bigred::kTerrainTexelWorldSize;
            const int gx = static_cast<int>(std::floor(fgx)), gz = static_cast<int>(std::floor(fgz));
            const float fx = fgx - static_cast<float>(gx), fz = fgz - static_cast<float>(gz);
            auto at = [&](int a, int b) {
                a = std::max(0, std::min(map_w - 1, a));
                b = std::max(0, std::min(map_h - 1, b));
                return static_cast<float>(mph_data[static_cast<size_t>(b) * map_w + a]) * height_scale;
            };
            const float n00 = at(gx, gz), n01 = at(gx, gz + 1), n10 = at(gx + 1, gz), n11 = at(gx + 1, gz + 1);
            return fx < fz ? n00 + fz * (n01 - n00) + fx * (n11 - n01)
                           : n00 + fx * (n10 - n00) + fz * (n11 - n10);
        };
        constexpr float kDrapeStep = 128.0f, kDrapeBand = 256.0f, kDrapeLift = 4.0f;
        // A local point of a draped shape, placed and raised - to_world with the ground check.
        auto draped_world = [&](float lx, float ly, float lz) -> std::array<float, 3> {
            const float gy = -ly;
            const float rx = rot[0] * lx + rot[1] * gy + rot[2] * lz;
            float up = -(rot[3] * lx + rot[4] * gy + rot[5] * lz) + ground_y;
            const float rz = rot[6] * lx + rot[7] * gy + rot[8] * lz;
            const float ground = terrain_at(rx + inst.x, rz + inst.z);
            if (up < ground + kDrapeLift && up > ground - kDrapeBand) up = ground + kDrapeLift;
            return {(static_cast<float>(map_w - 1) * bigred::kTerrainTexelWorldSize - (rx + inst.x)) *
                        world_to_render_scale,
                    up * world_to_render_scale, (rz + inst.z) * world_to_render_scale};
        };

        if (shape) {
            float lmin[3] = {1e9f,1e9f,1e9f}, lmax[3] = {-1e9f,-1e9f,-1e9f};
            float wmin[3] = {1e9f,1e9f,1e9f}, wmax[3] = {-1e9f,-1e9f,-1e9f};
            int ntris = 0;
            std::vector<LocalTri> drape_tris;   // the draped shape's kept triangles, by threes
            for (const LocalTri& t : *shape) {
                // Pick exactly one flat design alternative per placed instance - see LocalTri's
                // comment. Stride 1 (unlike the textured path's stride-2 front/back pairing):
                // a flat alternative's colour reads the same from either winding since nothing
                // here backface-culls, so there is no separate "back" member to skip past.
                if (face_owned_elsewhere(t.face_index)) continue;
                if (reached) {
                    if (t.face_index < 0 || static_cast<size_t>(t.face_index) >= reached->size() ||
                        !(*reached)[static_cast<size_t>(t.face_index)]) {
                        continue;
                    }
                } else if (t.alt_group >= 0 &&
                           t.alt_index != std::min<int>(variant, t.alt_count - 1)) {
                    continue;
                }
                if (drape) {
                    drape_tris.push_back(t);
                    continue;
                }
                std::array<float, 3> w = to_world(t.x + t.lx, local_y(t.vtx, t.y) + t.ly, t.z + t.lz);
                if (dbg) {
                    lmin[0]=std::min(lmin[0],t.x); lmin[1]=std::min(lmin[1],t.y); lmin[2]=std::min(lmin[2],t.z);
                    lmax[0]=std::max(lmax[0],t.x); lmax[1]=std::max(lmax[1],t.y); lmax[2]=std::max(lmax[2],t.z);
                    wmin[0]=std::min(wmin[0],w[0]); wmin[1]=std::min(wmin[1],w[1]); wmin[2]=std::min(wmin[2],w[2]);
                    wmax[0]=std::max(wmax[0],w[0]); wmax[1]=std::max(wmax[1],w[1]); wmax[2]=std::max(wmax[2],w[2]);
                    ++ntris;
                }
                bigred::TerrainVertex tv{};
                tv.x = w[0]; tv.y = w[1]; tv.z = w[2];
                tv.r = t.r; tv.g = t.g; tv.b = t.b;
                tv.br = t.br; tv.bg = t.bg; tv.bb = t.bb;
                tv.has_texture = 0.0f;
                tv.cull = t.one_sided ? 1.0f : 0.0f;
                out.push_back(tv);
            }
            // The draped shape: each triangle cut into n x n by barycentric steps, n from its
            // longest edge, every point placed by draped_world.
            for (size_t k = 0; k + 2 < drape_tris.size(); k += 3) {
                const LocalTri* v[3] = {&drape_tris[k], &drape_tris[k + 1], &drape_tris[k + 2]};
                float p[3][3];
                for (int i = 0; i < 3; ++i) {
                    p[i][0] = v[i]->x + v[i]->lx;
                    p[i][1] = v[i]->y + v[i]->ly;
                    p[i][2] = v[i]->z + v[i]->lz;
                }
                float longest = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float dx = p[i][0] - p[(i + 1) % 3][0], dz = p[i][2] - p[(i + 1) % 3][2];
                    longest = std::max(longest, std::sqrt(dx * dx + dz * dz));
                }
                // Only cut where the ground can reach it: the highest terrain node under the
                // triangle's footprint (one cell of margin) against its lowest corner. A roof
                // 800 units up - CITY's BLOCK1 - stays three vertices.
                float corner_up = 1e30f, xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
                for (int i = 0; i < 3; ++i) {
                    const float gy = -p[i][1];
                    const float rx = rot[0] * p[i][0] + rot[1] * gy + rot[2] * p[i][2] + inst.x;
                    const float rz = rot[6] * p[i][0] + rot[7] * gy + rot[8] * p[i][2] + inst.z;
                    corner_up = std::min(corner_up, -(rot[3] * p[i][0] + rot[4] * gy + rot[5] * p[i][2]) + ground_y);
                    xmin = std::min(xmin, rx); xmax = std::max(xmax, rx);
                    zmin = std::min(zmin, rz); zmax = std::max(zmax, rz);
                }
                float top = -1e30f;
                const int gx0 = static_cast<int>(std::floor(xmin / bigred::kTerrainTexelWorldSize)) - 1;
                const int gx1 = static_cast<int>(std::floor(xmax / bigred::kTerrainTexelWorldSize)) + 2;
                const int gz0 = static_cast<int>(std::floor(zmin / bigred::kTerrainTexelWorldSize)) - 1;
                const int gz1 = static_cast<int>(std::floor(zmax / bigred::kTerrainTexelWorldSize)) + 2;
                for (int gz = std::max(0, gz0); gz <= std::min(map_h - 1, gz1); ++gz)
                    for (int gx = std::max(0, gx0); gx <= std::min(map_w - 1, gx1); ++gx)
                        top = std::max(top, static_cast<float>(mph_data[static_cast<size_t>(gz) * map_w + gx]) * height_scale);
                const bool reachable = top + kDrapeLift > corner_up;
                const int n = !reachable ? 1
                              : std::max(1, std::min(32, static_cast<int>(std::ceil(longest / kDrapeStep))));
                auto point = [&](int a, int b) {   // barycentric (a/n, b/n, 1 - a/n - b/n)
                    const float wa = static_cast<float>(a) / n, wb = static_cast<float>(b) / n;
                    const float wc = 1.0f - wa - wb;
                    return draped_world(p[0][0] * wa + p[1][0] * wb + p[2][0] * wc,
                                        p[0][1] * wa + p[1][1] * wb + p[2][1] * wc,
                                        p[0][2] * wa + p[1][2] * wb + p[2][2] * wc);
                };
                auto put = [&](const std::array<float, 3>& w) {
                    bigred::TerrainVertex tv{};
                    tv.x = w[0]; tv.y = w[1]; tv.z = w[2];
                    tv.r = v[0]->r; tv.g = v[0]->g; tv.b = v[0]->b;
                    tv.br = v[0]->br; tv.bg = v[0]->bg; tv.bb = v[0]->bb;
                    tv.has_texture = 0.0f;
                    tv.cull = v[0]->one_sided ? 1.0f : 0.0f;
                    out.push_back(tv);
                };
                // Same winding as the source triangle: vertex 0 is (n,0), 1 is (0,n), 2 is (0,0).
                for (int a = 0; a < n; ++a)
                    for (int b = 0; a + b < n; ++b) {
                        put(point(a + 1, b)); put(point(a, b + 1)); put(point(a, b));
                        if (a + b + 1 < n) {
                            put(point(a + 1, b)); put(point(a + 1, b + 1)); put(point(a, b + 1));
                        }
                    }
            }
            if (dbg) {
                std::fprintf(stderr, "[diag]   FLAT mesh ntris=%d local_min=(%.1f,%.1f,%.1f) local_max=(%.1f,%.1f,%.1f) world_min=(%.2f,%.2f,%.2f) world_max=(%.2f,%.2f,%.2f)\n",
                             ntris, lmin[0],lmin[1],lmin[2], lmax[0],lmax[1],lmax[2],
                             wmin[0],wmin[1],wmin[2], wmax[0],wmax[1],wmax[2]);
            }
        }

        if (tex_info) {
            // UV source, per face, in priority order:
            //   1. the face's OWN UVs, when it has them - either inline (tag=5 full form) or
            //      inherited from the run's current crop (see threedw.cpp's stateful-crop note).
            //      This is how the bridges (BR_END/BR_MID/FOOT_BR) and the banners get each
            //      face's correct slice of a shared texture.
            //   2. otherwise the shape's variant list, indexed by MAP.DAT offset-19 - the
            //      per-instance selector used by ROADSIGN (13 turn-arrow/message crops, its own
            //      faces carry no UVs) and by CLOUDS (whose crops are listed after its faces, so
            //      nothing is inherited).
            // Getting this order wrong is what put a sliver of the wrong texture along the
            // bridge deck: BR_END gained a variant list from the tail scan, so the old
            // variants-win branch stretched a single crop across every face and threw away the
            // per-face UVs entirely.
            // Which faces this instance actually draws, and which crop the chosen design wants
            // if it carries no inline UV of its own - see select_textured_faces.
            int crop_index = variant;
            std::vector<bool> draw_face =
                select_textured_faces(*tex_info, variant, &crop_index);
            if (reached) {
                // The display list's own answer supersedes select_textured_faces' heuristics -
                // except for back sides, which it legitimately reaches and we cannot draw (see
                // TexturedFaceLocal::is_back_face).
                for (size_t fi = 0; fi < draw_face.size(); ++fi) {
                    const int idx = tex_info->faces[fi].face_index;
                    draw_face[fi] = idx >= 0 && static_cast<size_t>(idx) < reached->size() &&
                                    (*reached)[static_cast<size_t>(idx)] &&
                                    !tex_info->faces[fi].is_back_face;
                }
            }
            for (size_t fi = 0; fi < draw_face.size(); ++fi) {
                if (face_owned_elsewhere(tex_info->faces[fi].face_index)) draw_face[fi] = false;
            }
            // The crop the shape-wide fallback would use. A face with no inline UV of its own
            // prefers the crop the REPLAY says was current when control flow reached it - see
            // DisplayListReplay::crop. That is the original's own rule: a crop record is an
            // instruction, and the branches choose which one runs. This is what makes SNOW1's
            // seven ROADSIGN arrows seven different arrows instead of seven copies of one.
            const size_t fallback_vi =
                tex_info->variants.empty() ? 0
                    : std::min<size_t>(static_cast<size_t>(std::max(0, crop_index)),
                                       tex_info->variants.size() - 1);

            for (size_t fi = 0; fi < tex_info->faces.size(); ++fi) {
                if (!draw_face[fi]) continue;
                const TexturedFaceLocal& tf = tex_info->faces[fi];
                if (tf.pos.size() < 3) continue;
                const bool own_uv = (tf.uv.size() == tf.pos.size());
                if (!own_uv && tex_info->variants.empty()) continue;
                size_t vi = fallback_vi;
                if (!own_uv && replay && tf.face_index >= 0 &&
                    static_cast<size_t>(tf.face_index) < replay->crop.size()) {
                    const int rc = replay->crop[static_cast<size_t>(tf.face_index)];
                    if (rc >= 0 && static_cast<size_t>(rc) < tex_info->variants.size()) {
                        vi = static_cast<size_t>(rc);
                    }
                }

                // Unlike terrain's atlas (session 22, CONFIRMED V needed flipping), decor UV
                // does NOT need a V-flip - tested directly on ROADSIGN's "go straight" arrow
                // (variant 7): flipping V rendered the arrow upside-down, unflipped is correct.
                float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
                if (!own_uv) {
                    const bigred::UVVariant& uvv = tex_info->variants[vi];
                    u0 = uvv.u0 / bigred::kTmWidth; v0 = uvv.v0 / bigred::kTmHeight;
                    u1 = uvv.u1 / bigred::kTmWidth; v1 = uvv.v1 / bigred::kTmHeight;
                }
                // The crop's OWN corner list, when it has one entry per face vertex. This is
                // the real record; the rect below is only its bounding box.
                //
                // ⚠️ It carries the ORIENTATION, and dropping it cost a defect: ROADSIGN's crops
                // 1 and 2 are the same rectangle with the corners mirrored - a left arrow and a
                // right arrow from one bitmap - so reduced to a rect they became the same image.
                // That is why SNOW1's seven directional signs all pointed the same way.
                const std::vector<std::array<float, 2>>* crop_uv =
                    (!own_uv && !tex_info->variants.empty() &&
                     tex_info->variants[vi].uv.size() == tf.pos.size())
                        ? &tex_info->variants[vi].uv : nullptr;

                // Fallback mapping only: stretch the crop rect over the face's vertex loop
                // (u0,v0)->(u1,v0)->(u1,v1)->(u0,v1), which assumes a planar, roughly
                // rectangular face.
                static constexpr float kCornerU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                static constexpr float kCornerV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

                std::vector<bigred::TerrainVertex> corners;
                for (size_t i = 0; i < tf.pos.size(); ++i) {
                    // The stacking lift goes on AFTER ground conforming, not baked into the
                    // face: a conformed shape has its y replaced from the heightmap, which would
                    // otherwise throw the separation away and bring the z-fighting straight back.
                    // kDecalLayerLift is in raw world units and is tiny next to a 1024-wide grid
                    // slab - two thousandths of its width - so it separates without being seen.
                    constexpr float kDecalLayerLift = 2.0f;
                    const int layer = inst_layer + tf.decal_layer;
                    std::array<float, 3> w = to_world(
                        tf.pos[i][0] + tf.lx,
                        local_y(i < tf.vtx.size() ? tf.vtx[i] : -1, tf.pos[i][1]) + tf.ly
                            + static_cast<float>(layer) * kDecalLayerLift,
                        tf.pos[i][2] + tf.lz);
                    if (dbg) {
                        std::fprintf(stderr, "[diag]   face fi=%zu vtx=%zu local=(%.1f,%.1f,%.1f) world=(%.2f,%.2f,%.2f)\n",
                                     fi, i, tf.pos[i][0], tf.pos[i][1], tf.pos[i][2], w[0], w[1], w[2]);
                    }
                    bigred::TerrainVertex tv{};
                    tv.x = w[0]; tv.y = w[1]; tv.z = w[2];
                    if (own_uv) {
                        tv.u = tf.uv[i][0] / bigred::kTmWidth;
                        tv.v = tf.uv[i][1] / bigred::kTmHeight;
                    } else if (crop_uv) {
                        tv.u = (*crop_uv)[i][0] / bigred::kTmWidth;
                        tv.v = (*crop_uv)[i][1] / bigred::kTmHeight;
                    } else {
                        size_t c = i % 4;
                        tv.u = u0 + (u1 - u0) * kCornerU[c];
                        tv.v = v0 + (v1 - v0) * kCornerV[c];
                    }
                    tv.layer = static_cast<float>(
                        own_uv ? tex_info->layer
                               : tex_info->layer_for_variant(static_cast<int>(vi)));
                    tv.has_texture = 1.0f;
                    tv.cull = (tf.is_back_face || tf.has_back) ? 0.0f : 1.0f;
                    corners.push_back(tv);
                }
                for (size_t i = 1; i + 1 < corners.size(); ++i) {
                    out.push_back(corners[0]);
                    out.push_back(corners[i]);
                    out.push_back(corners[i + 1]);
                }
            }
        }
    }

    if (!unshaped.empty()) {
        std::vector<bigred::TerrainVertex> markers =
            build_marker_mesh(unshaped, mph_data, map_w, map_h, world_to_render_scale, height_scale, absolute_y);
        out.insert(out.end(), markers.begin(), markers.end());
    }
    return out;
}
} // namespace

bool LevelScene::load_shade_table(const std::string& game_dir) {
    // DEFAULT.CG - the distance-shading table, global to the whole game (it lives in ALWAYS.WAP,
    // not in any level) and used to derive the bright end of every palette index's ramp. Loaded
    // once: it is level-independent, only the MAP.COL it is applied to changes.
    //
    // Missing or malformed means no lighting, not no level - brighten_palette degrades to the
    // identity and the world renders exactly as it did before this existed.
    shade_table.clear();
    if (const Blob* cg = find_member(*game_archive_or_empty(game_dir, "ALWAYS.WAP"), "DEFAULT.CG"))
        shade_table = *cg;
    if (shade_table.size() < kShadeTableSize) {
        std::fprintf(stderr, "[shading] DEFAULT.CG not found in ALWAYS.WAP (or wrong size): "
                              "distance lighting disabled, colours render raw\n");
        shade_table.clear();
        return false;
    }
    return true;
}

bool LevelScene::load(const std::string& game_dir, const std::string& name) {
    const std::string path = game_path(game_dir, name + ".LEV");
    std::unordered_map<std::string, Blob> files;
    try {
        files = *game_archive(game_dir, name + ".LEV");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Cannot read '%s': %s\n", path.c_str(), e.what());
    }
    return load_files(name, std::move(files), 0, 0, path);
}

bool LevelScene::load_files(const std::string& name, std::unordered_map<std::string, Blob> files,
                            int want_w, int want_h, const std::string& origin) {
    lev.clear();
    palette.clear();
    palette_bright.clear();
    mph_data = mpc_data = cht_data = nullptr;
    cht_size = 0;
    map_w = map_h = 0;
    atlas_rgb.clear();
    atlas_rgb_bright.clear();
    atlas_count = 0;
    instances.clear();
    shape_names_by_slot.clear();
    shapes_by_slot.clear();
    shape_tex_by_slot.clear();
    shape_probes_by_slot.clear();
    shape_branches_by_slot.clear();
    shape_empty_by_slot.clear();
    anims.clear();
    anim_step_bufs.clear();
    sprites.clear();

    try {
    lev = std::move(files);

    auto mph_it = lev.find("MAP.MPH");
    auto mpc_it = lev.find("MAP.MPC");
    auto col_it = lev.find("MAP.COL");
    auto cht_it = lev.find("MAP.CHT");
    auto txt_it = lev.find(name + ".TXT");
    if (mph_it == lev.end() || mpc_it == lev.end() || col_it == lev.end()) {
        throw std::runtime_error("MAP.MPH/MAP.MPC/MAP.COL not all found in " + origin);
    }
    if (mph_it->second.size() != mpc_it->second.size()) {
        throw std::runtime_error("MAP.MPH/MAP.MPC size mismatch");
    }
    // Every shipped level is a 256x256 map (65536 bytes),
    // and the size is not stored anywhere, so a map given no size is taken as square. A
    // generated one says its own; MAP.MPH must then hold exactly that many bytes.
    if (want_w > 0 && want_h > 0) {
        if (static_cast<size_t>(want_w) * static_cast<size_t>(want_h) != mph_it->second.size())
            throw std::runtime_error("MAP.MPH does not hold " + std::to_string(want_w) + "x" +
                                     std::to_string(want_h) + " texels");
        map_w = want_w;
        map_h = want_h;
    } else {
        map_w = map_h = static_cast<int>(std::sqrt(static_cast<double>(mph_it->second.size())));
    }
    mph_data = mph_it->second.data();
    mpc_data = mpc_it->second.data();

    palette = bigred::load_palette_rgb(col_it->second.data(), col_it->second.size());
    // Same indices, read at the bright end of their ramp. DEFAULT.CG is global but MAP.COL
    // is per-level, so this has to be redone on every level load.
    palette_bright = bigred::brighten_palette(palette, shade_table.data(), shade_table.size());

    // Ground texture atlases: parse the level's own `texes <NAME> <N>` script line, load
    // each derived TEX*.TM file, decode through the palette, and stack them as consecutive
    // layers of one GL_TEXTURE_2D_ARRAY upload buffer.
    if (txt_it != lev.end()) {
        std::string txt(txt_it->second.begin(), txt_it->second.end());
        // Decor animation: the script's `anim` lines drive the variant byte of every instance
        // whose category routes to them - see ../formats/level_script.h.
        anims = bigred::parse_anim_directives(txt);
        if (!anims.empty()) {
            std::printf("%s.TXT: %zu ligne(s) anim", name.c_str(), anims.size());
            for (const bigred::LevelAnim& a : anims) {
                std::printf(" [%d pas x %d ms, masque 0x%02x]", a.step_count(), a.step_ms(), a.mask);
            }
            std::printf("\n");
        }
        if (auto texes = bigred::parse_texes_directive(txt)) {
            for (const std::string& name : bigred::texes_atlas_filenames(*texes)) {
                auto tm_it = lev.find(name + ".TM");
                if (tm_it == lev.end() || tm_it->second.size() != bigred::kTmSize) {
                    std::fprintf(stderr, "  (texes: %s.TM missing/wrong size, stopping atlas load at %d)\n",
                                 name.c_str(), atlas_count);
                    break;
                }
                std::vector<uint8_t> rgb = bigred::decode_tm_rgba(tm_it->second.data(), palette);
                atlas_rgb.insert(atlas_rgb.end(), rgb.begin(), rgb.end());
                std::vector<uint8_t> lit = bigred::decode_tm_rgba(tm_it->second.data(), palette_bright);
                atlas_rgb_bright.insert(atlas_rgb_bright.end(), lit.begin(), lit.end());
                ++atlas_count;
            }
            std::printf("Loaded %d ground texture atlas(es) from '%s %d'\n",
                        atlas_count, texes->base_name.c_str(), texes->count);
        }
    }

    cht_data = (cht_it != lev.end()) ? cht_it->second.data() : nullptr;
    cht_size = (cht_it != lev.end()) ? cht_it->second.size() : 0;

    // Scenery placement (MAP.DAT positions + shape index, MAP.SHL shape list). Both position
    // and shape identity are now CONFIRMED (byte 0 of each record directly indexes
    // MAP.SHL, matching the original's exact resolution formula).
    auto dat_it = lev.find("MAP.DAT");
    auto grp_it = lev.find("MAP.GRP");
    auto shl_it = lev.find("MAP.SHL");
    if (dat_it != lev.end()) {
        const uint8_t* grp_data = (grp_it != lev.end()) ? grp_it->second.data() : nullptr;
        size_t grp_size = (grp_it != lev.end()) ? grp_it->second.size() : 0;
        instances = bigred::parse_map_dat(dat_it->second.data(), dat_it->second.size(), grp_data, grp_size);

        if (shl_it != lev.end()) {
            std::vector<std::string> shl_names = bigred::parse_map_shl(shl_it->second.data(), shl_it->second.size());
            shape_names_by_slot = shl_names;
            g_diag_slot.assign(shl_names.size(), false);
            if (!g_diag_shape_filter.empty()) {
                for (size_t i = 0; i < shl_names.size(); ++i) {
                    if (g_diag_shape_filter == "*" ||
                        shl_names[i].find(g_diag_shape_filter) != std::string::npos) {
                        g_diag_slot[i] = true;
                        std::printf("[diag] tracing shape slot %zu = %s\n", i, shl_names[i].c_str());
                    }
                }
            }
            int loaded = 0, textured_shapes = 0;
            // Sheets pulled in by a CROP's own texture pointer rather than by a shape's embedded
            // name. Cached per level: several shapes name the same one, and an atlas layer is
            // 256x192x4 bytes twice over (plain and lit).
            std::unordered_map<std::string, int> crop_tm_layer;
            for (const std::string& name : shl_names) {
                auto three_it = lev.find(name + ".3DW");
                if (three_it == lev.end()) {
                    shapes_by_slot.emplace_back();
                    shape_tex_by_slot.emplace_back();
                    shape_empty_by_slot.push_back(false);
                    continue;
                }
                try {
                    bigred::ThreeDW shape_model = bigred::parse_3dw(three_it->second.data(), three_it->second.size());
                    // Same filter as the per-instance traces: --diag=NAME. Was hardcoded to
                    // "TUNN" from the tunnel investigation, which made it useless for anything else.
                    // "*" matches every shape - for surveys across a whole level.
                    if (!g_diag_shape_filter.empty() &&
                        (g_diag_shape_filter == "*" || name.find(g_diag_shape_filter) != std::string::npos)) {
                        int shape_idx = static_cast<int>(shapes_by_slot.size());
                        // `sprites` and `stop_tag` are on this line because they are what tells a
                        // shape with crops but no textured face apart: a shape that stopped early
                        // has faces it never reached, one that parsed clean has crops nothing
                        // consumes. That is the whole question on MSIGN1/MMARKER & co.
                        std::fprintf(stderr, "[shape-diag] shape=%s variants=%zu tex_name=%s"
                                             " sprites=%zu faces=%d/%d clean_end=%d stop_tag=%d"
                                             " @%zu\n",
                                     name.c_str(), shape_model.texture_variants.size(),
                                     shape_model.embedded_texture_name.c_str(),
                                     shape_model.sprites.size(), shape_model.parsed_face_count,
                                     shape_model.expected_face_count,
                                     shape_model.reached_clean_end ? 1 : 0, shape_model.stop_tag,
                                     shape_model.stop_byte_offset);
                        if (!shape_model.branches.empty()) {
                            for (int vv = 0; vv < 4; ++vv) {
                                std::vector<bool> reach = bigred::faces_reached(shape_model, vv);
                                std::fprintf(stderr, "[shape-diag]  rejeu variante %d -> faces [", vv);
                                for (size_t k = 0; k < reach.size(); ++k) {
                                    if (reach[k]) std::fprintf(stderr, "%zu,", k);
                                }
                                std::fprintf(stderr, "]\n");
                            }
                        }
                        for (const bigred::BranchMarker& b : shape_model.branches) {
                            std::fprintf(stderr, "[shape-diag]  branche tag=%d @%d : op=%d valeur=%d "
                                                  "-> vrai @%d, faux @%d\n",
                                         b.tag, b.pos, b.op, b.value, b.target, b.next);
                        }
                        std::fprintf(stderr, "[shape-diag] shape=%s (shape_index=%d) vertices=%zu faces=%zu\n",
                                     name.c_str(), shape_idx, shape_model.vertices.size(), shape_model.faces.size());
                        for (const bigred::SceneryInstance& inst : instances) {
                            if (inst.shape_index == shape_idx) {
                                std::fprintf(stderr, "[shape-diag]  instance pos=(%.0f,%.0f,%.0f) angles=(%d,%d,%d) variant=%d\n",
                                             inst.x, inst.y, inst.z, inst.angle_x, inst.angle_y, inst.angle_z, inst.variant_index);
                            }
                        }
                        for (size_t fi = 0; fi < shape_model.faces.size(); ++fi) {
                            const bigred::Face& f = shape_model.faces[fi];
                            std::fprintf(stderr, "[shape-diag]  face %zu: group=%d tex_id=%d n=%zu color=%s textured_variant=%d indices=[",
                                         fi, f.group, f.tex_id, f.indices.size(),
                                         f.color.has_value() ? std::to_string(*f.color).c_str() : "none",
                                         f.textured_variant);
                            for (int32_t idx : f.indices) std::fprintf(stderr, "%d,", idx);
                            std::fprintf(stderr, "] uv=[");
                            for (const std::array<float, 2>& t : f.uv) {
                                std::fprintf(stderr, "(%.0f,%.0f)", t[0], t[1]);
                            }
                            std::fprintf(stderr, "]\n");
                            for (int32_t idx : f.indices) {
                                const bigred::Vec3i& v = shape_model.vertices[static_cast<size_t>(idx)];
                                std::fprintf(stderr, "[shape-diag]    v%d = (%d,%d,%d)\n", idx, v.x, v.y, v.z);
                            }
                        }
                    }
                    // Base geometry plus EVERY extra group, always. ROADSIGN's three groups
                    // (1, 14, 21) are genuine sub-parts - a frame, its arrow overlay, and a
                    // second frame - and only including the first silently lost the arrow icon
                    // on every sign in the game (corrected 2026-09-13).
                    //
                    // CORRECTED 2026-09-15: this used to single out "animated" shapes and build
                    // them one tag=9 group per frame. That was wrong twice over - the groups are
                    // not the frames (FLAG has 8 poses across 4 groups, so each frame drew two
                    // poses at once), and whether a shape animates is not a property of its
                    // geometry at all but of the level script.
                    std::vector<int> base_groups{0};
                    {
                        const std::vector<int> extra = bigred::frame_groups(shape_model);
                        base_groups.insert(base_groups.end(), extra.begin(), extra.end());
                    }
                    shapes_by_slot.push_back(build_shape_local_mesh(shape_model, palette, palette_bright, base_groups));
                    shape_empty_by_slot.push_back(shape_model.faces.empty());
                    shape_probes_by_slot.resize(shapes_by_slot.size());
                    shape_probes_by_slot.back() = shape_model.ground_probes;
                    // What the display list needs to decide, per instance, which faces are
                    // reached - see place_scenery's faces_reached_for.
                    shape_branches_by_slot.resize(shapes_by_slot.size());
                    {
                        LevelScene::ShapeBranches sb;
                        sb.branches = shape_model.branches;
                        sb.face_stream_pos = shape_model.face_stream_pos;
                        sb.crop_stream_pos = shape_model.crop_stream_pos;
                        sb.face_count = shape_model.faces.size();
                        // Resolve each billboard's anchor vertex into the mesh convention now,
                        // while the parsed model is still here - see ShapeSprite.
                        for (const bigred::Sprite& sp : shape_model.sprites) {
                            if (sp.vertex < 0 ||
                                static_cast<size_t>(sp.vertex) >= shape_model.vertices.size()) continue;
                            const bigred::Vec3i& v = shape_model.vertices[static_cast<size_t>(sp.vertex)];
                            constexpr float sc = bigred::kModelVertexScale;
                            ShapeSprite ss;
                            ss.x = static_cast<float>(v.x) * sc;
                            ss.y = -static_cast<float>(v.y) * sc;
                            ss.z = static_cast<float>(v.z) * sc;
                            ss.vtx = sp.vertex;
                            // The record holds a HALF-extent ALREADY IN TRUE UNITS: unlike a
                            // vertex it does NOT carry the .3DW's 2x storage scale, because the
                            // original reads it after the transform has already halved the
                            // vertices. Applying `sc` here made every destruction filmstrip half
                            // the size of the object it replaces - MSIGN1/2 are 300 tall and
                            // their sprite is 2x150, anchored at mid-height, so it covers the
                            // sign exactly.
                            ss.half_w = static_cast<float>(sp.width);
                            ss.half_h = static_cast<float>(sp.height);
                            ss.crop = sp.crop;
                            ss.stream_pos = sp.stream_pos;
                            sb.sprites.push_back(ss);
                        }
                        shape_branches_by_slot.back() = std::move(sb);
                    }
                    if (!shapes_by_slot.back().empty()) ++loaded;

                    ShapeTextureInfo tex_info;
                    // A shape needs a texture if it has EITHER selectable variant crops
                    // (ROADSIGN etc, picked per-instance by MAP.DAT offset-19) OR textured
                    // faces carrying their own inline UVs (START/HELIPAD/STEPS - see
                    // Face::uv). Before the second case was handled, those shapes loaded no
                    // texture and rendered nothing, leaving every banner invisible.
                    bool has_own_uv = false;
                    for (const bigred::Face& fc : shape_model.faces) {
                        if (fc.textured_variant && fc.uv.size() == fc.indices.size() &&
                            fc.indices.size() >= 3) { has_own_uv = true; break; }
                    }
                    // A shape can need its texture for BILLBOARDS ALONE, with no textured face at
                    // all: SPIRE's 35 faces are every one of them flat-coloured, and its only
                    // textured elements are the two tag=13 lamps. Gating the atlas on textured
                    // faces left those shapes with layer -1, so no light was ever placed.
                    const bool has_sprites = !shape_model.sprites.empty();
                    if (!shape_model.texture_variants.empty() || has_own_uv || has_sprites) {
                        tex_info.faces = build_textured_faces_local(shape_model, base_groups);
                        tex_info.variants = shape_model.texture_variants;
                        // The real `.TM` reference is the shape's OWN embedded name string,
                        // NOT its MAP.SHL name - CONFIRMED session 23: BILBORD2.3DW embeds
                        // "BIL2" (-> BIL2.TM), S_LIGHTS.3DW embeds "GRASS" (-> a shared/
                        // generic atlas, unrelated to the shape's own name), ROADSIGN.3DW
                        // embeds "ROADSIGN" (matches here, but that's a coincidence - see
                        // ThreeDW::embedded_texture_name). Falls back to the MAP.SHL name if
                        // no embedded name was found, on the offchance some shape needs it.
                        // Appended as extra layers after the terrain ground atlases in the
                        // same shared GL_TEXTURE_2D_ARRAY (identical 256x192 format - see
                        // texture.h).
                        const std::string& tm_name =
                            shape_model.embedded_texture_name.empty() ? name : shape_model.embedded_texture_name;
                        // ⚠️ ONE shape in the whole game embeds the name WITH its extension:
                        // `SIGN1.3DW` (WATER1/2/3) carries "WBITS.TM", not "WBITS". Appending
                        // ".TM" made that "WBITS.TM.TM", no atlas was loaded, and the three water
                        // circuits' direction signs came out blank. Every other shape in every
                        // archive embeds a bare stem, so this is a tolerance for one authoring
                        // slip in the original data, not a second naming convention.
                        auto tm_it = lev.find(ends_with_tm(tm_name) ? tm_name : tm_name + ".TM");
                        if (g_verbose_logs) {
                            std::fprintf(stderr, "[diag] shape=%s tm_name=%s found=%d size_ok=%d faces_empty=%d\n",
                                         name.c_str(), tm_name.c_str(), tm_it != lev.end(),
                                         tm_it != lev.end() && tm_it->second.size() == bigred::kTmSize,
                                         tex_info.faces.empty());
                        }
                        if (tm_it != lev.end() && tm_it->second.size() == bigred::kTmSize &&
                            (!tex_info.faces.empty() || has_sprites)) {
                            std::vector<uint8_t> rgb = bigred::decode_tm_rgba(tm_it->second.data(), palette);
                            atlas_rgb.insert(atlas_rgb.end(), rgb.begin(), rgb.end());
                            std::vector<uint8_t> lit = bigred::decode_tm_rgba(tm_it->second.data(), palette_bright);
                            atlas_rgb_bright.insert(atlas_rgb_bright.end(), lit.begin(), lit.end());
                            tex_info.layer = atlas_count++;
                            ++textured_shapes;
                        }
                        // PER-CROP SHEETS. A crop record carries a pointer to the material it
                        // reads from, and it is not always the shape's first one: ROADSIGN's
                        // turn arrows are on `ROADSIGN.TM`, its destruction filmstrip on
                        // `SIGN.TM`. Four shapes in the game do this - ROADSIGN, MSIGN1, MSIGN2
                        // and CHPOST - and all four are destructibles whose flipbook lives on a
                        // sheet of its own. Drawing them from the shape's sheet is what put
                        // billboards and road markings on DIRT1's breaking signs.
                        tex_info.variant_layers.assign(tex_info.variants.size(), -1);
                        for (size_t vi = 0; vi < tex_info.variants.size(); ++vi) {
                            const std::string& ct = tex_info.variants[vi].texture;
                            if (ct.empty() || ct == tm_name) continue;
                            auto cached = crop_tm_layer.find(ct);
                            if (cached == crop_tm_layer.end()) {
                                auto ct_it = lev.find(ends_with_tm(ct) ? ct : ct + ".TM");
                                const int l = (ct_it != lev.end())
                                                  ? add_atlas_texture(ct_it->second, /*emissive=*/false)
                                                  : -1;
                                cached = crop_tm_layer.emplace(ct, l).first;
                                if (g_verbose_logs) {
                                    std::fprintf(stderr,
                                                 "[diag] crop sheet %s -> layer %d (named by %s)\n",
                                                 ct.c_str(), l, name.c_str());
                                }
                            }
                            tex_info.variant_layers[vi] = cached->second;
                        }
                        if (g_verbose_logs && !tex_info.variants.empty()) {
                            for (size_t vi = 0; vi < tex_info.variants.size(); ++vi) {
                                const bigred::UVVariant& uvv = tex_info.variants[vi];
                                std::fprintf(stderr,
                                             "[diag]   %s crop %zu -> %s (couche %d) "
                                             "(%.0f,%.0f)-(%.0f,%.0f)\n",
                                             name.c_str(), vi,
                                             uvv.texture.empty() ? "?" : uvv.texture.c_str(),
                                             tex_info.layer_for_variant(static_cast<int>(vi)),
                                             uvv.u0, uvv.v0, uvv.u1, uvv.v1);
                            }
                        }
                    } else if (g_verbose_logs) {
                        std::fprintf(stderr, "[diag] shape=%s no_variants_and_no_own_uv (flat_faces=%zu)\n",
                                     name.c_str(), shapes_by_slot.back().size());
                    }
                    shape_tex_by_slot.push_back(std::move(tex_info));
                } catch (const std::exception&) {
                    shapes_by_slot.emplace_back();
                    shape_tex_by_slot.emplace_back();
                    shape_empty_by_slot.push_back(false);
                }
            }
            // Keep every slot-indexed vector the same length whatever branch above ran.
            shape_probes_by_slot.resize(shapes_by_slot.size());
            shape_branches_by_slot.resize(shapes_by_slot.size());
            std::printf("%s.LEV: loaded %d/%zu MAP.SHL shapes with real geometry, %d with a "
                        "selectable texture\n", name.c_str(), loaded, shl_names.size(), textured_shapes);
        }
    }

    // In this order, and the order is load-bearing: the decal layering is the O(n^2) prepass
    // every pass borrows, the wreck list decides which instances the STATIC mesh must leave
    // out, and the wreck buffer appends its billboards to the sprite list rebuild_vbuf() owns.
    decal_layers = compute_decal_layers(instances, shape_tex_by_slot);
    identify_wrecks();
    identify_start_lights();
    rebuild_vbuf();
    rebuild_anim_bufs();
    rebuild_wreck_buf();
    rebuild_start_lights();
    load_horizon_objects(name);
    // Shared by both executables, so it must not mention the viewer's calibration keys.
    std::printf("%s.LEV: %zu vertices total (height_scale=%.1f)\n",
                name.c_str(), vbuf.size(), height_scale);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to load terrain from '%s': %s\n"
                              "(usage: openbigred [game_dir] [LEVEL_NAME] [--verbose|-v])\n",
                     origin.c_str(), e.what());
        return false;
    }
    return true;
}

// Each object is its shape's FIRST
// billboard (SUNRISE and BACKDROP are one tag=13 record each, no face), at the original's offset:
//     x = d cos(pitch) sin(yaw),  y = d sin(pitch),  z = d cos(pitch) cos(yaw)   (+Y down)
// plus the anchor vertex. The object's own angles (0,0,0 on both lines) are not applied - a
// billboard faces the camera anyway. ⚠️ The original clamps y to a range whose bounds are
// not known; unclamped here.
void LevelScene::load_horizon_objects(const std::string& name) {
    horizon_objects.clear();
    auto txt = lev.find(name + ".TXT");
    if (txt == lev.end()) return;
    const std::vector<bigred::HorizonObjectDef> defs =
        bigred::parse_horizon_objects(std::string(txt->second.begin(), txt->second.end()));
    for (const bigred::HorizonObjectDef& d : defs) {
        auto shape = lev.find(d.shape + ".3DW");
        if (shape == lev.end()) continue;
        const bigred::ThreeDW m = bigred::parse_3dw(shape->second.data(), shape->second.size());
        if (m.sprites.empty()) {
            std::fprintf(stderr, "horizobj %s: no billboard in the shape - not drawn\n", d.shape.c_str());
            continue;
        }
        const bigred::Sprite& sp = m.sprites.front();
        if (sp.crop < 0 || static_cast<size_t>(sp.crop) >= m.texture_variants.size()) continue;
        const bigred::UVVariant& uv = m.texture_variants[static_cast<size_t>(sp.crop)];
        const std::string sheet = !uv.texture.empty() ? uv.texture : m.embedded_texture_name;
        auto tm = lev.find(ends_with_tm(sheet) ? sheet : sheet + ".TM");
        if (tm == lev.end()) continue;
        const int layer = add_atlas_texture(tm->second, /*emissive=*/true);
        if (layer < 0) continue;
        constexpr double kBamToRad = 6.283185307179586 / 65536.0;
        const double a = d.pitch * kBamToRad, b = d.yaw * kBamToRad;
        double gx = d.dist * std::cos(a) * std::sin(b);
        double gy = d.dist * std::sin(a);
        double gz = d.dist * std::cos(a) * std::cos(b);
        if (sp.vertex >= 0 && static_cast<size_t>(sp.vertex) < m.vertices.size()) {
            const bigred::Vec3i& v = m.vertices[static_cast<size_t>(sp.vertex)];
            gx += v.x * bigred::kModelVertexScale;
            gy += v.y * bigred::kModelVertexScale;
            gz += v.z * bigred::kModelVertexScale;
        }
        HorizonObject h;
        // Game space to render space: X mirrored, +Y up.
        h.dx = static_cast<float>(-gx) * kRenderScale;
        h.dy = static_cast<float>(-gy) * kRenderScale;
        h.dz = static_cast<float>(gz) * kRenderScale;
        h.half_w = static_cast<float>(sp.width) * kRenderScale;
        h.half_h = static_cast<float>(sp.height) * kRenderScale;
        h.u0 = uv.u0 / bigred::kTmWidth; h.v0 = uv.v0 / bigred::kTmHeight;
        h.u1 = uv.u1 / bigred::kTmWidth; h.v1 = uv.v1 / bigred::kTmHeight;
        h.layer = static_cast<float>(layer);
        horizon_objects.push_back(h);
        std::printf("horizobj %s: %d units at yaw %d, pitch %d, %dx%d, sheet %s\n", d.shape.c_str(),
                    d.dist, d.yaw, d.pitch, sp.width * 2, sp.height * 2, sheet.c_str());
    }
}

std::vector<TerrainVertex> LevelScene::horizon_quads(const float cam[3], const float right[3],
                                                     const float up[3]) const {
    std::vector<TerrainVertex> out;
    for (const HorizonObject& h : horizon_objects) {
        static constexpr float kSx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        static constexpr float kSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static constexpr float kV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        TerrainVertex c[4];
        for (int i = 0; i < 4; ++i) {
            TerrainVertex v{};
            v.x = cam[0] + h.dx + right[0] * kSx[i] * h.half_w + up[0] * kSy[i] * h.half_h;
            v.y = cam[1] + h.dy + right[1] * kSx[i] * h.half_w + up[1] * kSy[i] * h.half_h;
            v.z = cam[2] + h.dz + right[2] * kSx[i] * h.half_w + up[2] * kSy[i] * h.half_h;
            v.u = h.u0 + (h.u1 - h.u0) * kU[i];
            v.v = h.v0 + (h.v1 - h.v0) * kV[i];
            v.layer = h.layer;
            v.has_texture = 1.0f;
            c[i] = v;
        }
        out.push_back(c[0]); out.push_back(c[1]); out.push_back(c[2]);
        out.push_back(c[0]); out.push_back(c[2]); out.push_back(c[3]);
    }
    return out;
}

int LevelScene::add_atlas_texture(const Blob& tm, bool emissive) {
    if (tm.size() != bigred::kTmSize || palette.empty()) return -1;
    std::vector<uint8_t> rgb = bigred::decode_tm_rgba(tm.data(), palette);
    atlas_rgb.insert(atlas_rgb.end(), rgb.begin(), rgb.end());
    if (emissive) {
        atlas_rgb_bright.insert(atlas_rgb_bright.end(), rgb.begin(), rgb.end());
    } else {
        std::vector<uint8_t> lit = bigred::decode_tm_rgba(tm.data(), palette_bright);
        atlas_rgb_bright.insert(atlas_rgb_bright.end(), lit.begin(), lit.end());
    }
    return atlas_count++;
}

int LevelScene::add_atlas_rgba(std::vector<uint8_t> rgba) {
    if (rgba.size() != bigred::kTmSize * 4) return -1;
    atlas_rgb_bright.insert(atlas_rgb_bright.end(), rgba.begin(), rgba.end());
    atlas_rgb.insert(atlas_rgb.end(), rgba.begin(), rgba.end());
    return atlas_count++;
}

std::vector<TerrainVertex> LevelScene::build_world_mesh(float terrain_hscale,
                                                       size_t* terrain_vertices_out,
                                                       std::vector<SpritePlacement>* sprites_out) const {
    std::vector<bigred::TerrainVertex> nv = bigred::build_terrain_mesh(
        mph_data, mpc_data, cht_data, cht_size, atlas_count, map_w, map_h, palette,
        palette_bright, kRenderScale, terrain_hscale);
    if (terrain_vertices_out) *terrain_vertices_out = nv.size();
    if (sprites_out) sprites_out->clear();
    std::vector<bigred::TerrainVertex> decor =
        place_scenery(instances, shapes_by_slot, shape_tex_by_slot, shape_probes_by_slot,
                       shape_branches_by_slot, mph_data, map_w, map_h,
                       kRenderScale, bigred::kHeightWorldScale, decor_absolute_y,
                       shape_empty_by_slot, /*draw_markers=*/true,
                       instance_passes(/*slot=*/-1, 0), sprites_out, &decal_layers);
    nv.insert(nv.end(), decor.begin(), decor.end());
    return nv;
}

// Splits every animated instance's faces between the static mesh and the animation steps.
//
// The original has no such split: it replays one display list per frame with whatever the
// variant byte currently holds, and the flagpole simply happens to be on every path. We build
// meshes once, so the faces the whole cycle AGREES on go in the static buffer and each step's
// buffer carries only what that step adds. The two sets are disjoint by construction, which is
// what keeps a flag from drawing its pole eight times over.
//
// `slot < 0` builds the static pass; otherwise it builds step `step` of `anims[slot]`.
std::vector<InstancePass> LevelScene::instance_passes(int slot, int step) const {
    std::vector<InstancePass> out(instances.size());
    const auto this_index_is_wreck = [this](size_t i) {
        return i < wreck_of_instance.size() && wreck_of_instance[i] >= 0;
    };
    for (size_t i = 0; i < instances.size(); ++i) {
        const bigred::SceneryInstance& inst = instances[i];
        InstancePass& p = out[i];
        p.variant = inst.variant_index;

        // A destructible instance is drawn from `wreck_buf`, never from these passes: its
        // appearance depends on race state, which a baked mesh cannot carry. A start light is
        // the same case for the same reason - see rebuild_start_lights().
        if (this_index_is_wreck(i)) { p.draw = false; continue; }
        if (std::find(start_lights.begin(), start_lights.end(), static_cast<int>(i)) !=
            start_lights.end()) {
            p.draw = false;
            continue;
        }

        const int inst_slot = bigred::anim_slot_of(inst.collision_category_byte,
                                                   inst.variant_index, inst.aux_byte);
        const bool animated = inst_slot >= 0 && static_cast<size_t>(inst_slot) < anims.size();
        if (!animated) {
            // Not animated: drawn whole by the static pass, absent from every animation pass.
            p.draw = (slot < 0);
            continue;
        }
        if (slot >= 0 && inst_slot != slot) { p.draw = false; continue; }

        const LevelAnim& a = anims[static_cast<size_t>(inst_slot)];
        // What each step of the cycle reaches, and the faces common to all of them.
        const auto variant_for = [&](int k) -> uint8_t {
            return static_cast<uint8_t>((inst.variant_index & ~a.mask) | a.values[static_cast<size_t>(k)]);
        };
        if (inst.shape_index < 0 ||
            static_cast<size_t>(inst.shape_index) >= shape_branches_by_slot.size()) {
            p.draw = (slot < 0);
            continue;
        }
        const ShapeBranches& sb = shape_branches_by_slot[static_cast<size_t>(inst.shape_index)];
        if (sb.branches.empty() || sb.face_count == 0) {
            // No conditionals: the variant byte changes nothing, so there is nothing to animate.
            p.draw = (slot < 0);
            continue;
        }
        std::vector<bool> common;
        for (int k = 0; k < a.step_count(); ++k) {
            std::vector<bool> r = bigred::faces_reached_from(sb.branches, sb.face_stream_pos,
                                                             sb.face_count, variant_for(k));
            if (k == 0) { common = std::move(r); continue; }
            for (size_t f = 0; f < common.size() && f < r.size(); ++f) {
                common[f] = common[f] && r[f];
            }
        }
        p.anim_slot = inst_slot;
        p.step_variants.reserve(static_cast<size_t>(a.step_count()));
        for (int k = 0; k < a.step_count(); ++k) p.step_variants.push_back(variant_for(k));
        if (slot < 0) {
            // Static pass: replay with step 0's variant, but keep only the common faces.
            p.variant = variant_for(0);
            p.skip_faces.assign(common.size(), false);
            for (size_t f = 0; f < common.size(); ++f) p.skip_faces[f] = !common[f];
        } else {
            const int k = a.step_count() > 0 ? ((step % a.step_count()) + a.step_count()) % a.step_count() : 0;
            p.variant = variant_for(k);
            p.skip_faces = common; // the static pass already drew these
        }
    }
    return out;
}

std::vector<TerrainVertex> LevelScene::build_anim_step(size_t slot, int step) const {
    return place_scenery(instances, shapes_by_slot, shape_tex_by_slot, shape_probes_by_slot,
                          shape_branches_by_slot, mph_data, map_w, map_h,
                          kRenderScale, bigred::kHeightWorldScale, decor_absolute_y,
                          shape_empty_by_slot, /*draw_markers=*/false,
                          instance_passes(static_cast<int>(slot), step), nullptr, &decal_layers);
}

// --------------------------------------------------------------------- destructible decor
//
// The two halves of the mechanism:
//
//   the SIMULATION decides an object is wrecked (category indices 4, 5 and 9) and rewrites
//   its variant byte; the DISPLAY LIST branches on that byte, so the whole change of appearance
//   is one number. Six shapes have no such test and carry two ENTRY POINTS instead, the damaged
//   one being the prologue that normal drawing skips.
//
// What we bake is every state each instance can be in, once, into one buffer; the frame then
// picks a draw range. Rebuilding a mesh when a cone goes over is exactly what the animated
// decor taught us not to do.
namespace {
// The identity of a replayed state: which faces, with which crop, and which billboards. Two
// states that agree on all of it draw the same thing and can share one baked range.
std::string replay_signature(const bigred::DisplayListReplay& r) {
    std::string sig;
    for (size_t i = 0; i < r.drawn.size(); ++i) {
        if (!r.drawn[i]) continue;
        sig += 'f';
        sig += std::to_string(i);
        sig += ':';
        sig += std::to_string(r.crop[i]);
    }
    for (size_t i = 0; i < r.sprite_drawn.size(); ++i) {
        if (!r.sprite_drawn[i]) continue;
        sig += 's';
        sig += std::to_string(i);
        sig += ':';
        sig += std::to_string(r.sprite_crop[i]);
    }
    return sig;
}
} // namespace

void LevelScene::identify_wrecks() {
    wrecks.clear();
    wreck_of_instance.assign(instances.size(), -1);

    for (size_t i = 0; i < instances.size(); ++i) {
        const SceneryInstance& inst = instances[i];
        const int cat = bigred::collision_category_index(inst.collision_category_byte);
        if (!bigred::category_is_destructible(cat)) continue;
        if (inst.shape_index < 0 ||
            static_cast<size_t>(inst.shape_index) >= shape_branches_by_slot.size()) continue;
        const ShapeBranches& sb = shape_branches_by_slot[static_cast<size_t>(inst.shape_index)];
        if (sb.branches.empty()) continue; // no control flow: one body, nothing to switch to

        std::vector<int32_t> spos;
        spos.reserve(sb.sprites.size());
        for (const ShapeSprite& sp : sb.sprites) spos.push_back(sp.stream_pos);
        const std::string intact = replay_signature(bigred::replay_display_list(
            sb.branches, sb.face_stream_pos, sb.crop_stream_pos, sb.face_count,
            inst.variant_index, spos, /*enter_at_prologue=*/false));

        bool differs = false;
        for (int k = 0; k < kWreckStepCount && !differs; ++k) {
            const uint8_t v = bigred::wrecked_variant(inst.variant_index, cat, k);
            differs = replay_signature(bigred::replay_display_list(
                          sb.branches, sb.face_stream_pos, sb.crop_stream_pos, sb.face_count, v,
                          spos, /*enter_at_prologue=*/true)) != intact;
        }
        // Destructible to the simulation but identical on screen either way: leave it in the
        // static mesh, where it costs nothing.
        if (!differs) continue;

        WreckPiece wp;
        wp.instance = static_cast<int>(i);
        wp.category = cat;
        wreck_of_instance[i] = static_cast<int>(wrecks.size());
        wrecks.push_back(wp);
    }
}

// ------------------------------------------------------------------ the start gantry's lights
//
// `S_LIGHTS` is the
// only decor piece whose appearance the RACE drives, and it does it the way everything else in
// this engine does: by rewriting the instance's variant byte, which the display list branches
// on. Baked once per state, like the animated and destructible decor, so changing the lights is
// a different draw range and never any buffer traffic.
void LevelScene::identify_start_lights() {
    start_lights.clear();
    for (size_t i = 0; i < instances.size(); ++i) {
        const int slot = instances[i].shape_index;
        if (slot < 0 || static_cast<size_t>(slot) >= shape_names_by_slot.size()) continue;
        // ⚠️ BY CATEGORY, NOT BY NAME: the start lights are category 0x35 (raw 53), and MOON3
        // has its own gantry, MLIGHTS, whose lamps test the same bits 1/2/4 of the variant byte
        // as S_LIGHTS. Matched by name, MOON3's lights never came on (user, 2026-09-28).
        if (instances[i].collision_category_byte == 53 ||
            shape_names_by_slot[static_cast<size_t>(slot)] == "S_LIGHTS")
            start_lights.push_back(static_cast<int>(i));
    }
}

void LevelScene::rebuild_start_lights() {
    lights_buf.clear();
    for (int k = 0; k < kStartLightCount; ++k) lights_ranges[k] = {0, 0};
    if (start_lights.empty()) return;

    for (int k = 0; k < kStartLightCount; ++k) {
        std::vector<InstancePass> passes(instances.size());
        for (InstancePass& p : passes) p.draw = false;
        for (int inst : start_lights) {
            InstancePass& p = passes[static_cast<size_t>(inst)];
            p.draw = true;
            // Only the low three bits are the lamps; whatever else the level put in the byte is
            // left alone, which is what the display list's own masked tests expect.
            p.variant = static_cast<uint8_t>((instances[static_cast<size_t>(inst)].variant_index &
                                              ~0x07) |
                                             kStartLightVariant[k]);
        }
        std::vector<TerrainVertex> mesh =
            place_scenery(instances, shapes_by_slot, shape_tex_by_slot, shape_probes_by_slot,
                          shape_branches_by_slot, mph_data, map_w, map_h, kRenderScale,
                          bigred::kHeightWorldScale, decor_absolute_y, shape_empty_by_slot,
                          /*draw_markers=*/false, passes, nullptr, &decal_layers);
        lights_ranges[k] = {static_cast<int>(lights_buf.size()), static_cast<int>(mesh.size())};
        lights_buf.insert(lights_buf.end(), mesh.begin(), mesh.end());
    }
}

void LevelScene::rebuild_wreck_buf() {
    wreck_buf.clear();
    for (WreckPiece& wp : wrecks) {
        wp.intact = {0, 0};
        wp.steps.assign(kWreckStepCount, {0, 0});
    }
    if (wrecks.empty()) return;

    // One pass per state, each drawing only the instances that need a NEW bake for it. A pass
    // emits each instance contiguously, so `ranges` comes straight back as the draw ranges.
    std::vector<std::pair<int, int>> ranges;
    auto run_pass = [&](const std::vector<InstancePass>& passes,
                        std::vector<SpritePlacement>* sprites_out) {
        std::vector<TerrainVertex> mesh =
            place_scenery(instances, shapes_by_slot, shape_tex_by_slot, shape_probes_by_slot,
                          shape_branches_by_slot, mph_data, map_w, map_h, kRenderScale,
                          bigred::kHeightWorldScale, decor_absolute_y, shape_empty_by_slot,
                          /*draw_markers=*/false, passes, sprites_out, &decal_layers, &ranges);
        const int base = static_cast<int>(wreck_buf.size());
        wreck_buf.insert(wreck_buf.end(), mesh.begin(), mesh.end());
        for (std::pair<int, int>& r : ranges) r.first += base;
    };

    // The variant byte each instance carries in each state, and the signature that goes with it.
    // Entry 0 is the intact object; 1 + k is destruction step k.
    const size_t n = wrecks.size();
    std::vector<std::vector<std::string>> sig(n);
    std::vector<std::vector<uint8_t>> variants(n);
    for (size_t w = 0; w < n; ++w) {
        const SceneryInstance& inst = instances[static_cast<size_t>(wrecks[w].instance)];
        const ShapeBranches& sb = shape_branches_by_slot[static_cast<size_t>(inst.shape_index)];
        std::vector<int32_t> spos;
        spos.reserve(sb.sprites.size());
        for (const ShapeSprite& sp : sb.sprites) spos.push_back(sp.stream_pos);
        variants[w].push_back(inst.variant_index);
        for (int k = 0; k < kWreckStepCount; ++k) {
            variants[w].push_back(
                bigred::wrecked_variant(inst.variant_index, wrecks[w].category, k));
        }
        for (size_t k = 0; k < variants[w].size(); ++k) {
            sig[w].push_back(replay_signature(bigred::replay_display_list(
                sb.branches, sb.face_stream_pos, sb.crop_stream_pos, sb.face_count,
                variants[w][k], spos, /*enter_at_prologue=*/k > 0)));
        }
    }

    // The intact pass is also the one that collects the billboards: a sprite has to know its
    // frame for EVERY state at once, since the quads are rebuilt per frame, not baked per state.
    {
        std::vector<InstancePass> passes(instances.size());
        for (InstancePass& p : passes) p.draw = false;
        for (size_t w = 0; w < n; ++w) {
            InstancePass& p = passes[static_cast<size_t>(wrecks[w].instance)];
            p.draw = true;
            p.variant = variants[w][0];
            p.enter_at_prologue = false;
            p.step_variants = variants[w];
            p.step_prologue.assign(variants[w].size(), 1);
            p.step_prologue[0] = 0;
            p.wreck_index = static_cast<int>(w);
        }
        run_pass(passes, &sprites);
        for (size_t w = 0; w < n; ++w) {
            wrecks[w].intact = ranges[static_cast<size_t>(wrecks[w].instance)];
        }
    }

    for (int k = 0; k < kWreckStepCount; ++k) {
        std::vector<InstancePass> passes(instances.size());
        for (InstancePass& p : passes) p.draw = false;
        std::vector<int> baking; // the pieces this pass actually draws
        for (size_t w = 0; w < n; ++w) {
            const std::string& want = sig[w][static_cast<size_t>(k) + 1];
            // Already baked? Share that range rather than emitting the same triangles again - a
            // cone that simply lies down has one state, not thirty-three.
            int share = -1;
            for (int j = 0; j < k && share < 0; ++j) {
                if (sig[w][static_cast<size_t>(j) + 1] == want) share = j;
            }
            if (share >= 0) {
                wrecks[w].steps[static_cast<size_t>(k)] = wrecks[w].steps[static_cast<size_t>(share)];
                continue;
            }
            if (want == sig[w][0]) {
                wrecks[w].steps[static_cast<size_t>(k)] = wrecks[w].intact;
                continue;
            }
            InstancePass& p = passes[static_cast<size_t>(wrecks[w].instance)];
            p.draw = true;
            p.variant = variants[w][static_cast<size_t>(k) + 1];
            p.enter_at_prologue = true;
            baking.push_back(static_cast<int>(w));
        }
        if (baking.empty()) continue;
        run_pass(passes, nullptr);
        for (int w : baking) {
            const size_t wi = static_cast<size_t>(w);
            wrecks[wi].steps[static_cast<size_t>(k)] =
                ranges[static_cast<size_t>(wrecks[wi].instance)];
        }
    }

    size_t faceless = 0;
    for (const WreckPiece& wp : wrecks) {
        if (wp.steps.back().second == 0) ++faceless;
    }
    std::printf("%zu destructible instance(s) with a second body, %zu of which lose every face "
                "(what is left of those IS the sprite animation)\n",
                wrecks.size(), faceless);
}

std::vector<std::pair<int, int>> LevelScene::wreck_draw_ranges(const SceneryState* state,
                                                                int32_t sim_tick) const {
    std::vector<std::pair<int, int>> out;
    for (const WreckPiece& wp : wrecks) {
        std::pair<int, int> r = wp.intact;
        if (state && state->is_wrecked(wp.instance)) {
            const int32_t since = sim_tick - state->wreck_tick[static_cast<size_t>(wp.instance)];
            r = wp.steps[static_cast<size_t>(wreck_step_at(since))];
        }
        if (r.second <= 0) continue;   // a state with no faces left - see the sprite path
        if (!out.empty() && out.back().first + out.back().second == r.first) {
            out.back().second += r.second;  // contiguous with the last one: one draw call
        } else {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<TerrainVertex> LevelScene::sprite_quads(const float right[3], const float up[3],
                                                    uint32_t elapsed_ms,
                                                    const SceneryState* state,
                                                    int32_t sim_tick) const {
    std::vector<TerrainVertex> out;
    out.reserve(sprites.size() * 6);
    for (const SpritePlacement& s : sprites) {
        if (s.frames.empty()) continue;
        size_t k = 0;
        if (s.wreck >= 0) {
            // A destructible's billboards: frame 0 is the intact object, 1 + step the flipbook.
            // This is the only thing left of MMARKER, MSIGN1/2, VMARKER and ARROW once they are
            // hit - their wrecked body has no faces at all.
            const int inst = (static_cast<size_t>(s.wreck) < wrecks.size())
                                 ? wrecks[static_cast<size_t>(s.wreck)].instance : -1;
            if (state && state->is_wrecked(inst)) {
                const int32_t since = sim_tick - state->wreck_tick[static_cast<size_t>(inst)];
                k = static_cast<size_t>(1 + wreck_step_at(since));
            }
            if (k >= s.frames.size()) k = s.frames.size() - 1;
        } else if (s.anim_slot >= 0 && s.frames.size() > 1) {
            k = static_cast<size_t>(anim_step_at(static_cast<size_t>(s.anim_slot), elapsed_ms)) %
                s.frames.size();
        }
        const SpritePlacement::Frame& f = s.frames[k];
        if (!f.on) continue; // off at this step
        // (-1,-1) (+1,-1) (+1,+1) (-1,+1), as two triangles.
        static constexpr float kSx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        static constexpr float kSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static constexpr float kV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        TerrainVertex corner[4];
        for (int i = 0; i < 4; ++i) {
            TerrainVertex v{};
            v.x = s.x + right[0] * kSx[i] * s.half_w + up[0] * kSy[i] * s.half_h;
            v.y = s.y + right[1] * kSx[i] * s.half_w + up[1] * kSy[i] * s.half_h;
            v.z = s.z + right[2] * kSx[i] * s.half_w + up[2] * kSy[i] * s.half_h;
            v.u = f.u0 + (f.u1 - f.u0) * kU[i];
            v.v = f.v0 + (f.v1 - f.v0) * kV[i];
            v.layer = (f.layer >= 0) ? static_cast<float>(f.layer) : s.layer;
            v.has_texture = 1.0f;
            corner[i] = v;
        }
        out.push_back(corner[0]); out.push_back(corner[1]); out.push_back(corner[2]);
        out.push_back(corner[0]); out.push_back(corner[2]); out.push_back(corner[3]);
    }
    return out;
}

std::vector<TerrainVertex> LevelScene::anim_packed(
    std::vector<std::vector<std::pair<int, int>>>* ranges) const {
    std::vector<TerrainVertex> all;
    if (ranges) ranges->assign(anim_step_bufs.size(), {});
    for (size_t s = 0; s < anim_step_bufs.size(); ++s) {
        for (const std::vector<TerrainVertex>& step : anim_step_bufs[s]) {
            if (ranges) {
                (*ranges)[s].emplace_back(static_cast<int>(all.size()), static_cast<int>(step.size()));
            }
            all.insert(all.end(), step.begin(), step.end());
        }
    }
    return all;
}

void LevelScene::rebuild_anim_bufs() {
    anim_step_bufs.clear();
    anim_step_bufs.resize(anims.size());
    for (size_t s = 0; s < anims.size(); ++s) {
        for (int k = 0; k < anims[s].step_count(); ++k) {
            anim_step_bufs[s].push_back(build_anim_step(s, k));
        }
    }
}

} // namespace bigred
