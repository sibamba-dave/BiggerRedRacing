// Drawing the ground-effect particles the simulation keeps in sim/particles.h.
//
// The split is the usual one: the
// FIELD is simulation (deterministic, integer, ticks), and everything here is a projection of it.
//
// A `spot` effect's shape is a FLIPBOOK, built exactly like the nitro flame - a few faces with
// no UV of their own, behind a branch tree over the animation counter whose leaves are crops of
// `SPOTFX.TM` / `SPOTFX2.TM`. So the frames bake once, at load, and a frame of a live particle
// is an index. Two shapes of flipbook exist and both are used:
//
//   * FACES - `DUST1_FX`, `SNOW1_FX`, `RUBBERFX`, `BIGSPL`, `SPLASH2`. Two or four quads crossed
//     in an X, standing on the ground. Crossed quads are view-independent by construction, which
//     is why the original never had to turn them towards the camera;
//   * SPRITES - `SPARK_FX`, which has ONE vertex, no face at all and four `tag=13` billboards.
//     Those are camera-facing and are rebuilt per frame, like the decor's (LevelScene).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../formats/bigred_archive.h"
#include "../formats/level_script.h"
#include "../sim/particles.h"
#include "vehicle_render.h"   // ParticleSprite, bake_fx_sprites
#include "level_scene.h"

namespace bigred {


// The baked visual of one `spot` line, parallel to the level's spot table.
struct ParticleKind {
    // Indexed by the particle's frame counter, 0 .. SpotEffect::frame_count.
    std::vector<std::vector<TerrainVertex>> by_frame;
    std::vector<std::vector<ParticleSprite>> sprites_by_frame;
    bool empty() const { return by_frame.empty() && sprites_by_frame.empty(); }
};

struct ParticleVisuals {
    std::vector<ParticleKind> kinds;  // one per SpotEffect, same order
    bool any() const {
        for (const ParticleKind& k : kinds) if (!k.empty()) return true;
        return false;
    }
};

// Bakes every `spot` effect of a level's environment.
//
// `env` is the unpacked `.ENV` (DIRT.ENV, CITY.ENV...) - the effect shapes and their sheets live
// there and nowhere else. ⚠️ `scene` is MUTATED: the sheets are appended to its texture array,
// so this has to run before the executable uploads it, exactly like build_vehicle_model.
ParticleVisuals build_particle_visuals(const std::vector<SpotEffect>& spots,
                                       const std::unordered_map<std::string, Blob>& env,
                                       LevelScene& scene);

// Every live particle, placed in the world and ready to stream. `right`/`up` are the camera's
// axes, for the sprite-only effects; the face-based ones ignore them.
std::vector<TerrainVertex> place_particles(const ParticleVisuals& visuals,
                                           const ParticleField& field,
                                           const float right[3], const float up[3],
                                           int map_w, float render_scale);

} // namespace bigred
