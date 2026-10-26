// A loaded level, ready to draw: terrain mesh, decor mesh, texture atlases, animation frames.
//
// This is everything both executables need and neither should own. `map_preview` is the
// standalone level viewer (free camera, live calibration keys); `openbigred` is the game. They
// share this so the world can never look different in one than in the other.
//
#pragma once

#include "../formats/bigred_archive.h"
#include "../formats/level_script.h"
#include "../formats/scenery.h"
#include "../formats/terrain.h"
#include "../formats/threedw.h"
#include "../sim/vehicle.h" // SceneryState - what the destruction state IS, and who owns it

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bigred {

// World units -> render units. Same factor for terrain, decor and vehicles, so proportions hold.
constexpr float kRenderScale = 1.0f / 300.0f;

// The near clipping plane, in WORLD units - multiply by kRenderScale for the projection.
// ⚠️ It used to be 1.0 render unit, i.e. 300 world units: more than half the chase distance and
// more than half a terrain cell, so every slope nearer the lens than that was cut away and the
// picture looked through the hillside (the user's report of 2026-09-23, with a drawing). It must
// stay under the chase camera's hard ground clearance (kHardClearance, chase_camera.cpp), or the
// ground under the lens is cut open again.
constexpr float kNearPlaneWorld = 10.0f;

// Per-instance/per-face "[diag]" traces. Off by default: printing them for every matching
// scenery instance to an unbuffered console is what made level loading feel very slow.
extern bool g_verbose_logs;

// Substring matched against MAP.SHL shape names at load time; every instance of a matching shape
// gets its local and world bounding box dumped to stderr. Set it before load(). Empty = off.
// This is how you answer "do these two instances overlap?" without guessing - it is how the
// starting grid z-fighting was investigated.
extern std::string g_diag_shape_filter;

// Whether the .3DW tag=11 lines are built into the decor (on by default). A switch for A/B
// comparisons at the bench - set it before load().
extern bool g_decor_lines;

// Decor geometry in LOCAL (un-placed, un-scaled) raw world units, fan-triangulated and
// palette-coloured. `r,g,b` is the raw authored colour and `br,bg,bb` the same palette index at
// the bright end of its shading ramp - see shading.h.
//
// `alt_group`/`alt_index`/`alt_count`: set when this triangle's face shares its exact vertex set
// with one or more OTHER flat faces of the same shape (a design alternative, not a texture-path
// duplicate). -1 means "no ambiguity, always draw". place_scenery picks exactly one alternative
// per PLACED INSTANCE from MAP.DAT offset-19, since different instances of the same shape can
// need different alternatives - CONFIRMED on CHEVRONS.3DW.
struct LocalTri {
    float x, y, z, r, g, b;
    float br, bg, bb;
    int alt_group = -1, alt_index = 0, alt_count = 1;
    int vtx = -1; // source vertex index, for ground conforming
    int face_index = -1; // index into ThreeDW::faces, so the display list's branches can decide
                          // per instance whether this triangle is reached at all
    // Local-space offset that separates this face from the coplanar ones it is painted over -
    // see coplanar_lifts. Zero for almost everything; non-zero on a sign's glyph, a plate's
    // border, anything the original drew flat on top of something else in a NON-horizontal
    // plane (the horizontal case is the ground-decal path instead).
    float lx = 0, ly = 0, lz = 0;
    // No other face of the shape runs over the same vertices the opposite way round: the
    // original shows it from one side only (TerrainVertex::cull).
    bool one_sided = false;
};

// One textured face of a shape, in local space.
struct TexturedFaceLocal {
    std::vector<std::array<float, 3>> pos;
    // Source vertex index per entry of `pos`, so ground conforming moves a textured face's
    // vertices exactly like it moves a flat one's.
    std::vector<int> vtx;
    // The face's OWN inline per-vertex UV (raw .TM texels), when it has one - see Face::uv.
    std::vector<std::array<float, 2>> uv;
    // How many coplanar faces of the same shape this one is stacked ON TOP OF. 0 for almost
    // everything. See build_textured_faces_local for how it is derived and why it exists: the
    // original paints its faces in queue order and never needed to separate them, we have a
    // depth buffer and they z-fight. place_scenery lifts the face by this many steps.
    int decal_layer = 0;
    // Same non-horizontal coplanar separation as LocalTri's - see coplanar_lifts. `decal_layer`
    // above stays the HORIZONTAL (ground-decal) mechanism; the two never apply to one face.
    float lx = 0, ly = 0, lz = 0;
    // Which tag=9 group the face came from (0 = before any marker).
    int group = 0;
    int face_index = -1; // index into ThreeDW::faces - see LocalTri::face_index
    // True when an EARLIER face of the same group uses the same vertex set with the opposite
    // winding: this one is its back side. The original draws both and backface-culls; we do not
    // cull, so drawing both leaves two exactly coplanar quads fighting. Dropped at emission.
    bool is_back_face = false;
    // True when a LATER face is this one's back side (the pair of is_back_face). A face with
    // neither is one-sided, and culled like the original's - TerrainVertex::cull.
    bool has_back = false;
};

// What one pass over the scenery instances should do differently, per instance. Used to split
// the world into a STATIC mesh (built once) and one small mesh per animation step: an animated
// instance's faces are partitioned between the two so neither draws the other's, and nothing is
// drawn twice. See LevelScene::build_anim_step.
struct InstancePass {
    bool draw = true;     // false: this instance contributes nothing to this pass
    uint8_t variant = 0;  // the variant byte to replay the shape's display list with
    // Faces the OTHER pass owns. Indexed by ThreeDW::faces index; empty means no restriction.
    std::vector<bool> skip_faces;
    // Which `anims[]` entry this instance follows (-1 = none), and the variant byte it carries at
    // each step of that cycle. Only the STATIC pass fills these, and only the sprite collection
    // reads them - a sprite's visibility has to be known for every step at once, since the quads
    // are rebuilt per frame rather than baked per step. See SpritePlacement.
    int anim_slot = -1;
    std::vector<uint8_t> step_variants;
    // Enter the shape's display list AT its 2-int prologue rather than past it. On the six
    // two-body shapes that have no 0x40 test (SHEEP, the plants, the signposts) that prologue
    // jumps straight to the DAMAGED body, so this is the whole of "draw it wrecked" for them.
    bool enter_at_prologue = false;
    // Parallel to `step_variants` when the steps do not all share `enter_at_prologue` - the
    // destruction flipbook's first entry is the intact object and the rest are the wreck.
    std::vector<uint8_t> step_prologue;
    // Which LevelScene::wrecks entry this instance is, for the sprites collected in this pass.
    int wreck_index = -1;
};

// A shape's `{13, ...}` billboard with its anchor vertex already resolved into the same local
// convention the meshes use (Y negated, halved) - see threedw.h's Sprite.
struct ShapeSprite {
    float x = 0, y = 0, z = 0;     // local position of the anchor vertex
    int vtx = -1;                  // its index, so ground conforming moves the sprite too
    float half_w = 0, half_h = 0;  // local half-size
    int crop = -1;                 // index into ShapeTextureInfo::variants
    int32_t stream_pos = 0;        // for the replay
};

// One `{13, vertex, width, height}` billboard, already placed in the world - see threedw.h's
// Sprite.
//
// The quad is NOT baked: the four corners are rebuilt every frame from the camera's right/up
// vectors, because the original's sprite is camera-facing with a size fixed in WORLD units.
// There are only ~140 of these in the whole game, all of them lights on the moon tracks, so
// rebuilding them costs nothing and the shared world shader stays untouched.
struct SpritePlacement {
    float x = 0, y = 0, z = 0;    // world centre, render units
    float half_w = 0, half_h = 0; // render units. The record holds a HALF-extent already -
                                   // CONFIRMED in the original, which lays the four corners at
                                   // centre +/- each - so it is scaled but NOT halved again.
    float layer = 0;
    int anim_slot = -1;           // -1 = never animated, one frame
    // Which LevelScene::wrecks entry drives this sprite, or -1. When set, `frames` is indexed
    // by the DESTRUCTION step and not by an `anim` slot: entry 0 is the intact object and
    // entry 1 + s is step s of the flipbook. That is how a marker that loses all its faces
    // when hit still shows something - what is left of it IS the sprite animation.
    int wreck = -1;
    // One entry per step of that slot (or exactly one when static). The UV varies per step, not
    // just the on/off: the branches pick a different CROP for the same sprite record, which is
    // how one record becomes an animation.
    struct Frame {
        bool on = false;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        // The atlas layer THIS frame reads from, or -1 for the record's own `layer`. A flipbook
        // can cross sheets, because each crop names its own - see ShapeTextureInfo.
        int layer = -1;
    };
    std::vector<Frame> frames;
};

// Per-shape texture-variant data (see threedw.h's UVVariant / scenery.h's offset-19 comment).
// `layer < 0` means no matching `.TM` was found, in which case these faces just aren't emitted.
struct ShapeTextureInfo {
    std::vector<TexturedFaceLocal> faces;
    std::vector<UVVariant> variants;
    int layer = -1;
    // Parallel to `variants`: the atlas layer THAT crop reads from, or -1 for "the shape's own
    // `layer`". A crop record names its own sheet (UVVariant::texture) and it is not always the
    // shape's first one - ROADSIGN's arrows are on `ROADSIGN.TM` while its destruction filmstrip
    // is on `SIGN.TM`. Drawing the filmstrip from the shape's sheet showed billboards and road
    // markings where the breaking sign should be.
    std::vector<int> variant_layers;
    // The layer a crop should be drawn from: its own when it has one, else the shape's.
    int layer_for_variant(int vi) const {
        if (vi >= 0 && static_cast<size_t>(vi) < variant_layers.size() &&
            variant_layers[static_cast<size_t>(vi)] >= 0)
            return variant_layers[static_cast<size_t>(vi)];
        return layer;
    }
};

// Everything one level contributes, plus the meshes built from it. Load once, draw from the
// buffers; `rebuild_*` exist for the viewer's live calibration keys.
struct LevelScene {
    // --- raw level data, owner of everything the pointers below point into ---
    std::unordered_map<std::string, Blob> lev;
    std::vector<RGB> palette;
    std::vector<RGB> palette_bright; // `palette` through DEFAULT.CG's brightest level
    const uint8_t* mph_data = nullptr;
    const uint8_t* mpc_data = nullptr;
    const uint8_t* cht_data = nullptr;
    size_t cht_size = 0;
    int map_w = 0, map_h = 0;

    // --- texture atlases, raw half then lit half (see the fragment shader) ---
    std::vector<uint8_t> atlas_rgb;
    std::vector<uint8_t> atlas_rgb_bright;
    int atlas_count = 0;

    // Appends one 256x192 `.TM` to the shared atlas as a new layer and returns its index, or -1
    // if the blob is not one. This is how something that is NOT part of the level - the nitro
    // flame sheet out of INGAME.WAP - gets into the same texture array as the decor, instead of
    // needing a second sampler and a second shader.
    //
    // ⚠️ Only valid BEFORE the executable uploads the atlas: until then it is a plain byte
    // vector, afterwards the GL texture is already sized.
    //
    // `emissive` puts the SAME decoded bytes in the lit half, which makes the fragment shader's
    // distance mix a no-op for this layer. A flame is a light source, not a surface being lit -
    // the same reasoning that excludes the vehicle body (see vehicle_render.cpp).
    int add_atlas_texture(const Blob& tm, bool emissive);

    // The same, for a sheet that has ALREADY been decoded - which a vehicle's has, because its
    // emblem cell is painted before upload (scene/vehicle_render.h). Always emissive, for the
    // reason add_atlas_texture's `emissive` exists: a vehicle is never distance-shaded.
    int add_atlas_rgba(std::vector<uint8_t> rgba);

    // --- decor ---
    std::vector<SceneryInstance> instances;
    std::vector<std::string> shape_names_by_slot; // MAP.SHL order, parallel to shapes_by_slot
    std::vector<std::vector<LocalTri>> shapes_by_slot;
    std::vector<ShapeTextureInfo> shape_tex_by_slot;
    // Per MAP.SHL slot: what the shape's compiled display list needs to decide, per instance,
    // which faces are actually reached. Kept instead of the whole parsed model.
    struct ShapeBranches {
        std::vector<BranchMarker> branches;
        std::vector<ShapeSprite> sprites; // the shape's tag=13 billboards - see SpritePlacement
        std::vector<int32_t> face_stream_pos;
        // Where each texture crop sits in the same stream - a crop is an instruction the control
        // flow reaches or jumps over, so which one applies is part of the replay, not a lookup
        // indexed by the variant byte. See DisplayListReplay::crop.
        std::vector<int32_t> crop_stream_pos;
        size_t face_count = 0;
    };
    std::vector<ShapeBranches> shape_branches_by_slot;

    std::vector<std::vector<Vec3i>> shape_probes_by_slot; // ground-conform probes
    std::vector<bool> shape_empty_by_slot;                 // parsed but declares no faces (CHECKP)

    // --- decor animation (see ../formats/level_script.h) ---
    // The level script's `anim` lines, in file order. An instance follows line
    // `anim_slot_of(category, variant, aux_byte)`, which is how one FLAG shape becomes eight
    // poses: the line rewrites the masked bits of the variant byte, and the shape's own display
    // list branches on exactly those bits.
    std::vector<LevelAnim> anims;
    // Per `anims` entry, the world geometry of each step of its cycle - only the faces that
    // step owns, everything the whole cycle has in common already being in `vbuf`.
    std::vector<std::vector<std::vector<TerrainVertex>>> anim_step_bufs;

    // Every billboard of the level, placed. Rebuilt into quads each frame by sprite_quads().
    std::vector<SpritePlacement> sprites;

    // The `horizobj` lines (formats/level_script.h): billboards kept at a fixed offset from
    // the camera, so they sit at the horizon however far the player drives - DIRT1's rising
    // sun, MOON3's Earth. Drawn after the sky and before the world, with no depth, as the
    // original paints them first.
    struct HorizonObject {
        float dx = 0, dy = 0, dz = 0;   // offset from the camera, render units
        float half_w = 0, half_h = 0;   // render units
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        float layer = 0;
    };
    std::vector<HorizonObject> horizon_objects;
    std::vector<TerrainVertex> horizon_quads(const float cam[3], const float right[3],
                                             const float up[3]) const;

    // --- destructible decor (see ../jeu/objets-destructibles.md) -------------------------
    //
    // One entry per instance a vehicle can knock over AND whose drawing actually changes when
    // it does. Those instances are NOT in `vbuf`: they are drawn from `wreck_buf`, one draw
    // range each, picked from the shared SceneryState every frame. Same mechanism as the decor
    // animation - a baked buffer and a choice of range, never a rebuilt mesh.
    struct WreckPiece {
        int instance = -1;                      // index into `instances`
        int category = 0;                       // its collision category INDEX - which of the
                                                 // two rewrites of the variant byte applies
        std::pair<int, int> intact{0, 0};       // {first vertex, count} in `wreck_buf`
        // One per step of the destruction flipbook, the last being the settled state. Steps
        // whose replay is identical share a range, so a cone that simply lies down costs one.
        std::vector<std::pair<int, int>> steps;
    };
    // --- the start gantry's lights -------------------------------------------------------
    //
    // `S_LIGHTS` is the traffic light that stands either side of the `START` banner (collision
    // category 0x35, a category of its own, two instances per start line). It is the game's
    // countdown: there is no number on screen, the lights go red then green.
    //
    // The shape draws the same NINE faces whatever its variant byte; what the byte chooses is
    // the CROP on the last of them - the lamp panel. Six crops, and the mapping is not a list
    // but a bit field: **bit 0 is the green lamp, bit 1 the amber, bit 2 the red**. Variants 5
    // (red+green) and 7 (all three) have no artwork and fall back to the dark panel, which is
    // exactly what you would expect of combinations the authors never drew.
    //
    //   0 = off   1 = green   2 = amber   3 = amber+green   4 = red   6 = red+amber
    //
    // Measured with physics_bench's LIGHTS block, which prints the crop each variant selects.
    enum class StartLight { Red = 0, RedAmber, Amber, Green, Count };
    static constexpr int kStartLightCount = static_cast<int>(StartLight::Count);
    // The variant byte each of those states writes, in the same order.
    static constexpr uint8_t kStartLightVariant[kStartLightCount] = {4, 6, 2, 1};

    std::vector<int> start_lights;                     // instance indices, empty if the level has none
    std::vector<TerrainVertex> lights_buf;             // every state of every one of them
    std::pair<int, int> lights_ranges[kStartLightCount]{};  // {first vertex, count} per state
    // Finds the instances and bakes the four states. Like the wreck buffer, those instances are
    // then NOT in `vbuf`: the frame picks a range instead of rebuilding a mesh.
    void identify_start_lights();
    void rebuild_start_lights();

    std::vector<WreckPiece> wrecks;
    std::vector<int> wreck_of_instance;   // parallel to `instances`, -1 where not destructible
    std::vector<TerrainVertex> wreck_buf; // built by rebuild_wreck_buf()

    // How many steps `WreckPiece::steps` holds: the 32 animated ones plus the settled state.
    static constexpr int kWreckStepCount = kWreckAnimSteps + 1;

    // Which step of the destruction is showing, `ticks` PHYSICS ticks after the crash. The
    // original's counter advances once per 14 ms game tick and our tick is 28 ms, so it moves
    // two steps a tick - and it is a duration either way, never a frame count.
    static int wreck_step_at(int32_t ticks_since) {
        if (ticks_since < 0) return kWreckStepCount - 1;
        const int64_t step = static_cast<int64_t>(ticks_since) * kPhysicsTickMs / kGameTickMs;
        return step >= kWreckStepCount - 1 ? kWreckStepCount - 1 : static_cast<int>(step);
    }

    // Finds the instances whose drawing actually changes when they are knocked over, and fills
    // `wrecks` / `wreck_of_instance`. Must run BEFORE rebuild_vbuf(), which reads the result to
    // leave those instances out of the static mesh.
    void identify_wrecks();
    // Bakes every state of every such instance into `wreck_buf` and appends their billboards to
    // `sprites`. Runs after rebuild_vbuf(), which owns (and clears) that sprite list.
    void rebuild_wreck_buf();

    // What to draw out of `wreck_buf` this frame, as {first vertex, count} ranges already
    // merged: nothing is wrecked at the start of a race, and the intact ranges are laid out in
    // instance order, so the common case comes back as ONE range. `state` null = all intact,
    // which is what the viewer wants.
    std::vector<std::pair<int, int>> wreck_draw_ranges(const SceneryState* state = nullptr,
                                                       int32_t sim_tick = 0) const;

    // The billboards visible `elapsed_ms` into the level, as camera-facing triangles ready to
    // draw with the world program. `right`/`up` are the camera's basis vectors in render space.
    //
    // `state`/`sim_tick` bring in the destruction state, so a sprite that belongs to a wrecked
    // object shows the right frame of its flipbook. Null state = nothing is wrecked.
    void load_horizon_objects(const std::string& name);
    std::vector<TerrainVertex> sprite_quads(const float right[3], const float up[3],
                                            uint32_t elapsed_ms,
                                            const SceneryState* state = nullptr,
                                            int32_t sim_tick = 0) const;

    // --- built meshes ---
    std::vector<TerrainVertex> vbuf;                  // terrain first, then static decor
    // How many leading vertices of `vbuf` are TERRAIN. The decor that follows is drawn as a
    // second range with a depth bias, because a decal lying flat on the ground (a helipad, a
    // start line) is exactly coplanar with it and z-fights otherwise - see the render loop.
    size_t vbuf_terrain_vertices = 0;

    // --- knobs ---
    float height_scale = kHeightWorldScale;
    // CONFIRMED session 22 (user A/B-tested live against real gameplay): decor sits at MAP.DAT's
    // own fixed (x,-y,z), NOT ground-snapped onto MAP.MPH. Still toggleable for comparison.
    bool decor_absolute_y = true;

    // The global shading table (DEFAULT.CG, from ALWAYS.WAP). Empty = no distance lighting, and
    // the world renders exactly as it did before that existed. Set by load_shade_table().
    Blob shade_table;

    // Reads ALWAYS.WAP for DEFAULT.CG. Call once, before load(); failure is not fatal.
    bool load_shade_table(const std::string& game_dir);

    // Replaces everything above with `name`.LEV. Returns false on any failure, having reset the
    // scene first, so a failed live switch leaves a clean empty scene rather than a half level.
    bool load(const std::string& game_dir, const std::string& name);
    // ... from a set of files already in memory, named as inside a .LEV - a generated circuit's,
    // or a level unpacked elsewhere (code review, A4). It needs what a .LEV holds: at least
    // MAP.MPH, MAP.MPC, MAP.COL and `name`.TXT; everything else is optional, as in a .LEV.
    // `map_w` x `map_h`: the map's size in texels, which MAP.MPH must match; 0 x 0 = square,
    // from MAP.MPH's size, which is what every shipped level is. `origin` names the files in
    // an error message.
    bool load_files(const std::string& name, std::unordered_map<std::string, Blob> files,
                    int map_w = 0, int map_h = 0, const std::string& origin = "(memory)");

    // Terrain + static decor, at a given terrain height scale. Decor is grounded on the FIXED
    // kHeightWorldScale whatever this is, so live calibration moves only the terrain's own shape.
    // `terrain_vertices_out`, when given, receives how many leading vertices are terrain.
    // `sprites_out`, when given, also receives the level's billboards (see SpritePlacement).
    std::vector<TerrainVertex> build_world_mesh(float terrain_height_scale,
                                                size_t* terrain_vertices_out = nullptr,
                                                std::vector<SpritePlacement>* sprites_out = nullptr) const;

    // The world geometry contributed by one step of one `anim` slot: the instances that follow
    // that slot, replayed with the variant byte the step imposes, MINUS the faces every step of
    // the cycle shares (those are static, and already in `vbuf`). Markers suppressed - in an
    // animation pass the hundreds of static instances legitimately contribute nothing, which is
    // not the "unknown shape" case.
    std::vector<TerrainVertex> build_anim_step(size_t slot, int step) const;

    // Per instance, what the static pass draws (`slot < 0`) or what one animation step draws.
    std::vector<InstancePass> instance_passes(int slot, int step) const;

    void rebuild_vbuf() { vbuf = build_world_mesh(height_scale, &vbuf_terrain_vertices, &sprites); }

    // Cross-instance decal layering, worked out once - see place_scenery. Filled by load().
    std::vector<int> decal_layers;
    void rebuild_anim_bufs();

    // `anim_step_bufs` flattened into one upload, with `ranges[slot][step]` the
    // {first vertex, vertex count} to draw for that step. Both executables use this so the
    // animation is switched by picking a draw range, never by touching a buffer.
    std::vector<TerrainVertex> anim_packed(
        std::vector<std::vector<std::pair<int, int>>>* ranges) const;

    // Which step of `anims[slot]` is showing `elapsed_ms` after the level started. Wall-clock,
    // never a frame count - see LevelAnim::step_at.
    int anim_step_at(size_t slot, uint32_t elapsed_ms) const {
        return slot < anims.size() ? anims[slot].step_at(elapsed_ms) : 0;
    }
};

} // namespace bigred
