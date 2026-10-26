// Drawing a simulated vehicle with the shared world shader.
//
// The body is pure flat-colour palette geometry - no texture. That is not a shortcut: the
// original bakes a 7-step luminance ramp of the team colour into the face indices and never
// lights a car at run time.
//
// The world transform is the SAME one the decor uses (level_scene.cpp's to_world): rotate in the
// game's axes where +Y points down, then mirror world X about (map_w - 1) * 512. A vehicle that
// used any other convention would drive on a different map than the one it is drawn on.
#pragma once

#include "../formats/bigred_archive.h"
#include "../formats/level_script.h"   // SunLight
#include "../formats/threedw.h"
#include "../formats/vehicle_script.h"
#include "../sim/vehicle.h"
#include "level_scene.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace bigred {

// THE TEAM RAMP, and it is the whole of how this game colours a vehicle.
//
// The original ships SIX repaints of every body
// (`JEEP1.3DW`..`JEEP6.3DW`), identical geometry, differing only in which palette indices their
// faces name. Each repaint owns a **block of SIXTEEN consecutive entries**, and the blocks start
// at 48 and run every 16: 48, 64, 80, 96, 112, 128. Within a block the entries are a smooth
// luminance ramp of one hue - red, green, olive, blue, orange, violet - and the faces are
// numbered along it to fake per-facet shading on very simple geometry. Nothing is lit at run
// time, and the original never recolours anything: it ships the six.
//
// ⚠️ ONLY THE BLOCK OF *THIS* REPAINT IS THE TEAM COLOUR. The other five blocks are ordinary
// palette ramps that a vehicle is free to paint with, and several do: `HELI1` names the red
// block 58..62 (its team colour), the OLIVE block 90..94, the blue 100 and 110 - and `HELI2`
// swaps only the first for 74..78, keeping the olive and the blues untouched. They are the
// livery, and index 100 in particular is the canopy glass, 38 faces of it. Treating every block
// as team colour turned the helicopter into one flat hue and painted its windscreen.
//
// ⚠️ AND ONLY OFFSETS 2..15 ARE THE PAINT. Offsets 0 and 1 are used by all six repaints alike -
// index 48 is a pure (255,0,0) that every variant of every vehicle names - so they are lights
// and trim, not bodywork. Established by taking the six variants of every shape in the game and
// keeping the indices that shift by exactly +16 from one to the next; offsets 2,3,6,8..15 come
// out of that test and nothing else does.
//
// ⚠️ AND THE SHARED GREYS ARE NOT PART OF IT EITHER: 27..30 and 45..47 are tyres, glass and
// chrome, identical in all six. A recolour reaching them would paint the tyres.
//
// ⚠️ THIS WAS "57..63, SEVEN ENTRIES" UNTIL 2026-09-23 and that was too narrow at both ends. The
// hovercraft's rudders paint with 56 and 58 - and with 72 and 74 on repaint 2, so both are team
// colours - which left half of every fin unrecoloured and red. The user reported it as "la
// moitie de ses ailerons rouge".
constexpr int kTeamRampFirst = 48;         // offset 0 of team 1's block
constexpr int kTeamRampStride = 16;        // from one team's block to the next
constexpr int kTeamRampSteps = 16;         // entries per block
constexpr int kTeamRampFirstPainted = 2;   // offsets below this are shared, never recoloured
constexpr int kTeamCount = 6;

// How dark the LAST step is, as a fraction of the first painted one. Averaged over the 30 blocks
// the shipped levels hold (6 teams x 5 palettes), where offsets 2..15 come out almost exactly
// linear: 1.000, 0.933, 0.874, 0.817, ... , 0.279, 0.228. So a generated ramp is that same
// straight line, which is why nothing here needs a table.
constexpr float kTeamRampFloor = 0.228f;

// A colour the PLAYER chose, which the six repaints need not contain.
//
// ⚠️ IT IS THE BRIGHTEST STEP, not an average: the generated ramp runs from it down to
// kTeamRampFloor of it, exactly as the authored ones run from their own first entry.
//
// ⚠️ AND IT REPLACES THE REPAINT RATHER THAN CHOOSING ONE. The design says it
// in as many words: do NOT re-implement "bake N copies of the model". One canonical geometry -
// variant 1 - and the ramp regenerated for whatever hue was picked. That is what makes twenty-two
// colours cost no data at all, and it is why `build_vehicle_model` takes this INSTEAD of a
// repaint number when it is set.
struct VehicleTint {
    uint8_t r = 0, g = 0, b = 0;
};

// THE LIGHTING OF A VEHICLE. Per emitted vertex of a flat-
// coloured face: its smooth normal in the shape's own game axes (+Y down, unit), and the palette
// index of its face - what the original walks up DEFAULT.CG by `sun intensity x cos(angle)`.
// `idx` -1: not lit (textured faces, the fallback colour, the driver's head).
struct VertexShade {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    int idx = -1;
};

// The light a vehicle is drawn under this frame.
struct VehicleLighting {
    float sun[3] = {0.0f, -1.0f, 0.0f};   // unit, game axes (+Y down), pointing TOWARDS the sun
    int intensity = 255;                  // 0..255 - the original's per-object byte, by distance
};

// The level's sun as that unit vector: the forward axis of a light object pitched `elevation`
// and turned `azimuth` (the same convention as a vehicle), reversed - the
// original lights a face whose SEEN side looks up it.
void sun_direction(const SunLight& s, float out[3]);

// The level byte the original gives an object at `distance` world units from the camera:
// `((cells - 2) * 0x200 - distance) >> 4`, clamped to 0..255.
int light_intensity(float distance);

// One rigid piece of a vehicle: a mesh in LOCAL render space (+Y up, .3DW halved), plus where it
// hangs off the body.
struct VehiclePart {
    std::vector<TerrainVertex> local;  // triangles, un-placed, un-scaled
    int32_t pos[3] = {0, 0, 0};        // attachment point, game axes, true world units
    // The `wheel` line's own rotation, all three angles - pitch, yaw, roll. Not just the yaw:
    // the shapes are all authored with their axle along their local Z, and the anchor is what
    // aims it. A wheel yaws a quarter turn to put its axle across the car; a main rotor PITCHES
    // a quarter turn to stand it vertical; a tail rotor yaws. Keeping only the yaw silently
    // laid every helicopter's main rotor flat.
    int32_t rot_bam16[3] = {0, 0, 0};
    int32_t steer_gain = 0;            // BAM16 of yaw per unit of the steering axis
    // `&82`, the cabin's steering wheel: its ROLL is minus the steering times the line's `size`,
    // replacing the line's own. 0 for every other part.
    int32_t roll_steer_gain = 0;
    bool rolls = false;                // spins with the road speed
    bool powered = false;              // spins on the engine's own clock: a rotor, a prop
    int32_t flags = 0;                 // the `wheel` line's role byte, kept as authored
    std::vector<VertexShade> shade;    // one per vertex of `local` - empty: not lit
};

// How many values of VehicleSim::flame_phase the flame is baked for. The counter is 0 when the
// boost is off and cycles 0x11..0x20 while it runs, so 0x21 entries cover every value it takes.
constexpr int kFlamePhaseCount = 0x21;

// A nitro flame emitter (`wheel` flag &87/&88) - ONE BAKED MESH PER ANIMATION PHASE.
//
// ⚠️ The flame is not one shape scaled up and down. NIT_FX.3DW is five tag=6 faces with no UV of
// their own, preceded by a display list that CALLS a branch tree over the animation counter; the
// tree's leaves are `tag=5` crop records, each one a 16x16 or 16x48 cell of SPOTFX3.TM's sprite
// sheet. So the counter selects a FRAME OF A FLIPBOOK, by exactly the mechanism that makes a
// ROADSIGN show one arrow out of fourteen. Baking one mesh per phase
// resolves the whole tree once, at load, and leaves the draw a plain index.
struct VehicleFx {
    std::vector<std::vector<TerrainVertex>> by_phase; // kFlamePhaseCount entries
    int32_t pos[3] = {0, 0, 0};                       // attachment, game axes, world units
    int32_t yaw_bam16 = 0;
};

// One camera-facing billboard of an effect, on one frame of its filmstrip.
struct ParticleSprite {
    float x = 0, y = 0, z = 0;      // anchor, local render space
    float half_w = 0, half_h = 0;   // render units - a HALF-extent already, see threedw.h
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float layer = -1;
};

// A damage effect (`wheel` flag &89..&8D): the engine fire, the three smokes, the explosion.
// Baked like the nitro flame, one
// mesh per value of the variant byte its display list branches on (8 are enough: every value
// the original gives it is `& 7` or at most 7).
struct VehicleDamageFx {
    int32_t flags = 0;                                 // &89 FLAMES, &8A..&8C SMOKE, &8D BOOM
    std::vector<std::vector<TerrainVertex>> by_phase;  // kDamageFxPhases entries
    std::vector<std::vector<ParticleSprite>> sprites_by_phase;  // BOOM is billboards only
    int32_t pos[3] = {0, 0, 0};                        // attachment, game axes, world units
};
constexpr int kDamageFxPhases = 8;

// Everything needed to draw one vehicle type.
struct VehicleModel {
    std::vector<TerrainVertex> body;   // local render space
    std::vector<VehiclePart> parts;    // wheels
    // The nitro flame emitters (`wheel` flag &87/&88), drawn only while the boost is running.
    // Their shape, NIT_FX, lives in INGAME.WAP - never in a `.VHC`.
    //
    // ⚠️ NOT here, and deliberately: `&89` (FLAMES), `&8A`/`&8B`/`&8C` (SMOKE) and `&8D` (BOOM).
    // They are not boost effects. Their anchors say so - on a TRUCK, FLAMES sits at z=+120 and
    // the first SMOKE at z=+130, both over the ENGINE, while the nitro jets sit at z=+17 behind
    // it. They are the damage fire, the damage smoke (thresholds 80/90/95 %) and the explosion,
    // and none of them can be triggered correctly until there is a damage model.
    std::vector<VehicleFx> nitro_fx;
    // The damage effects, and the wreck: `<NAME>X.3DW`, the body once destroyed, when the
    // vehicle ships one (a `vehicle <base> 7` line); empty keeps the painted body.
    std::vector<VehicleDamageFx> damage_fx;
    std::vector<TerrainVertex> wreck_body;
    // Each part's orientation on a wreck: the original throws three rand() at every part when
    // the car dies, and they stay. Rolled once per model here - drawing only.
    std::vector<std::array<int32_t, 3>> wreck_rot;
    // How far the body reaches from its origin, world units - what the damage effects are
    // pulled towards the camera by (place_vehicle_effects).
    float radius = 0.0f;
    // Where each `.TM` the shapes named landed (TextureSink), by sheet name - the cabin view
    // rewrites its dashboard's layer every frame (scene/dashboard.h).
    std::unordered_map<std::string, int> sheet_layers;
    // The lighting: one VertexShade per vertex of `body`, and what the lit indices are coloured
    // with - the race's builder only sets these (a menu has no DEFAULT.CG and stays unlit).
    std::vector<VertexShade> body_shade;
    std::vector<RGB> shade_palette;
    const Blob* shade_cg = nullptr;   // DEFAULT.CG, owned by the LevelScene
    bool shade_tinted = false;
    VehicleTint shade_tint{};
    int shade_colour = 1;
    bool valid() const { return !body.empty(); }
};

// A vehicle's orientation matrix, GAME axes (+Y down), from its three angles in radians - the
// original's own matrix, row-major. Local (x, y, z) goes to (m0 x + m1 y + m2 z, ...).
void vehicle_rotation(float pitch, float yaw, float roll, float m[9]);

// THE DRIVER'S HEAD, for the one anchor a `.VHC` cannot resolve by itself: `wheel &FF,&40,...
// ;head` - `&40` is "take it from the GLOBAL table", i.e. the head of the character the player
// picked (scene/character_model.h). Null leaves the anchor empty, which is what a vehicle with
// nobody known at the wheel gets.
//
// `mouth_tm` is the sheet its textured faces are cut from (`MOUTH.TM`, in `CHARSCN.SCN` like the
// heads themselves); `tint` is the CHARACTER's colour, not the vehicle's - the two are
// independent choices.
struct VehicleDriver {
    const ThreeDW* head = nullptr;
    const Blob* mouth_tm = nullptr;
    const VehicleTint* tint = nullptr;
};

// One shape as triangles in LOCAL render space (+Y up, .3DW halved), coloured through `palette`,
// its team-coloured faces walked onto `tint`'s ramp when there is one, its textured faces on
// atlas `layer` (-1: none). The very conversion a vehicle's body goes through - exposed for the
// character, whose body and head are shapes of their own and not a `.VHC`.
std::vector<TerrainVertex> shape_mesh(const ThreeDW& model, const std::vector<RGB>& palette,
                                      const VehicleTint* tint, int layer);

// One frame of an effect FLIPBOOK, in local render space - the shape's faces with the crop the
// display list reaches when the animation counter is `phase`. Shared by the nitro flame and by
// the ground-effect particles, which are built exactly alike (see scene/particle_render.h).
std::vector<TerrainVertex> bake_fx_phase(const ThreeDW& model, int layer, int phase);


// The same frame's `tag=13` billboards - for a shape with no faces at all (SPARK_FX, BOOM).
std::vector<ParticleSprite> bake_fx_sprites(const ThreeDW& model, int layer, int phase);

// Loads the body (`<BASE><colour>.3DW`) and every wheel-shaped attachment out of an unpacked
// `.VHC`, colouring faces through the scene's palettes exactly like the decor.
// `colour` is 1..6 (the six team repaints); out of range falls back to 1.
//
// `scene` is taken by reference and MUTATED: the flame's sprite sheet is appended to its atlas
// (LevelScene::add_atlas_texture), so this has to run before the executable uploads it.
// THE LOGO CELL, and it is the whole of how this game puts an emblem on a car.
//
// Every vehicle sheet is 256x192, and its bottom-right corner is a 3 x 2 grid of 40x40 emblems
// that fills it exactly: 136 + 3*40 = 256, 112 + 2*40 = 192. ONE CELL PER TEAM REPAINT. A
// body's logo slot is a face whose UV is precisely its own repaint's cell - `JEEP1` samples
// (136,112), `JEEP2` (176,112), `JEEP4` (136,152) - and the same six cells are used by the
// buggy, the hovercraft's rudders and every other vehicle. Measured 2026-09-23.
//
// ⚠️ SO CHOOSING A LOGO IS NOT A UV CHANGE. Thirteen choices could never fit a grid of six:
// the original paints the chosen emblem INTO the cell of the sheet before uploading it, and
// this engine does the same. The geometry never knows.
//
// ⚠️ AND THE CELL'S SURROUND IS TRANSPARENT (palette index 0), so the emblem is a DECAL over
// the bodywork panel, not a replacement for it - see shape_to_local.
constexpr int kLogoCell = 40;
constexpr int kLogoGridX = 136, kLogoGridY = 112, kLogoGridCols = 3;

// Where repaint `colour` (1..6) keeps its emblem, in raw `.TM` texels.
void logo_cell_origin(int colour, int& x, int& y);

// The emblem to paint there: `kLogoCell` x `kLogoCell` RGBA8, as ui/team_logos.h hands it over.
// An empty one leaves the sheet's own authored emblem alone.
struct VehicleLogo {
    const uint8_t* rgba = nullptr;
    int w = 0, h = 0;
    bool valid() const { return rgba && w == kLogoCell && h == kLogoCell; }
};

// Where a vehicle's OWN textures go. A `.VHC` ships its `.TM` files alongside its shapes -
// `BTWH.TM` is the Beetle's wheel, `MWHL.TM` the Mini's, `*DSH.TM` the dashboards, emblem grid
// and all - and the caller decides where an image lands: the race appends it to the level's
// atlas, a menu keeps its own little array. Returns the LAYER the faces should name, or -1.
//
// ⚠️ IT IS HANDED DECODED RGBA, not the raw `.TM`, precisely because the emblem has to be
// painted in first - and that must happen in exactly one place, or the menu and the race would
// each grow their own copy of it.
using TextureSink = std::function<int(std::vector<uint8_t>)>;

// `tint`, when given, replaces the repaint: the body is built from variant 1 and every face
// painted in ANY of the six team blocks is walked onto a ramp generated for that colour. Null
// keeps the authored repaint `colour` names, which is what a vehicle with no player behind it
// gets.
VehicleModel build_vehicle_model(const std::unordered_map<std::string, Blob>& vhc,
                                 const std::unordered_map<std::string, Blob>& ingame,
                                 const VehicleTuning& tune, int colour, LevelScene& scene,
                                 const VehicleTint* tint = nullptr,
                                 const VehicleLogo* logo = nullptr,
                                 const VehicleDriver* driver = nullptr, int section = 0);

// The same body and the same rigid parts, WITHOUT the effect emitters - what a MENU shows.
//
// A menu has no `LevelScene`: there is no atlas to append a flame's sprite sheet to, and a
// parked car has no boost running anyway. Everything else - the body, the wheels, the tracks,
// the rotors, their anchors and their angles - is built by the very same code as the race's,
// which is the point of having this overload rather than a second, simpler loader in ui/: a
// vehicle drawn in a menu cannot drift from the one the race draws.
//
// `palette` is the level's own MAP.COL, for exactly the same reason.
VehicleModel build_vehicle_model(const std::unordered_map<std::string, Blob>& vhc,
                                 const VehicleTuning& tune, int colour,
                                 const std::vector<RGB>& palette,
                                 const VehicleTint* tint = nullptr,
                                 const TextureSink& add_texture = nullptr,
                                 const VehicleLogo* logo = nullptr,
                                 const VehicleDriver* driver = nullptr, int section = 0);

// `section` 1 is the CABIN: `IN_<NAME><colour>` and its own parts (the steering wheel), what the
// cabin view draws around the eye. Invalid when the vehicle has none.

// Everything about a vehicle that the DRAWING depends on, in the game's own frame (+Y down,
// world X unmirrored) - and in floating point, because it is the thing that gets interpolated.
//
// ⚠️ This exists so the renderer never reads VehicleSim directly. The simulation advances in
// 28 ms steps; a display refreshing at 60, 144 or 240 Hz that drew the latest step would show
// the same pose two or seven frames running and then jump. Two consecutive poses and the
// fraction of a tick elapsed between them give smooth motion at any refresh rate, and - the
// point of the exercise - the SAME motion at any refresh rate.
struct VehiclePose {
    float x = 0.0f, y = 0.0f, z = 0.0f; // world units, fractional (the sim holds 1/512ths)
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f; // BAM16
    float steer = 0.0f;                 // +/-256, full lock
    float wheel_spin = 0.0f;            // BAM16, how far the wheels have rolled
    float rotor_spin = 0.0f;            // BAM16, how far the rotors and props have turned
    bool boost_active = false;
    uint8_t flame_phase = 0;
    // The damage, for the smoke (81/91/96 %) and the wreck. Discrete: taken from the later tick.
    int32_t damage_percent = 0;
    bool wrecked = false;
    uint32_t fx_clock = 0;     // the original's 14 ms timer ticks, which the smoke's frames read
    uint32_t wreck_clock = 0;  // timer ticks since the car died - the explosion's frames
};

// The pose a simulation tick leaves behind. Snapshot it BEFORE each step to keep the pair.
VehiclePose pose_of(const VehicleSim& sim);

// Between two consecutive ticks, `t` in [0, 1]. Angles take the short way round, and a jump too
// large to be motion (a `recover`) snaps to `b` instead of smearing across the map.
VehiclePose lerp_pose(const VehiclePose& a, const VehiclePose& b, float t);

// The DAMAGE EFFECTS of a vehicle - smoke, engine fire, explosion - placed in the world, apart
// from the vehicle itself, and drawn after it without face culling.
//
// ⚠️ PULLED TOWARDS THE CAMERA BY THE VEHICLE'S RADIUS. The original has no depth buffer: it
// paints a car's effect pieces AFTER its body (in a second pass), so its smoke always
// shows over the car. The data counts on it - a TRUCK's smoke is 40 units tall, anchored 20 over
// the chassis, i.e. INSIDE the cab. Here a depth test hid it entirely. Moving each vertex a car's
// radius nearer the eye puts it over its own car and still behind a hill. `cam`, `right` and
// `up` are the camera's position and axes, render space; the axes turn the explosion's
// billboards.
std::vector<TerrainVertex> place_vehicle_effects(const VehicleModel& model, const VehiclePose& pose,
                                                 const float cam[3], const float right[3],
                                                 const float up[3], int map_w,
                                                 float render_scale);

// The vehicle's triangles, placed in the world, ready to append to a stream-drawn buffer.
// `map_w` is the level's terrain width - it sets the X mirror axis.
// `light`, when given and the model carries its lighting, recolours every lit vertex.
std::vector<TerrainVertex> place_vehicle(const VehicleModel& model, const VehiclePose& pose,
                                         int map_w, float render_scale,
                                         const VehicleLighting* light = nullptr);

} // namespace bigred
