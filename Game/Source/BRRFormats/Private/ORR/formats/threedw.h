// .3DW object geometry parser.
//
// The format has a multi-block vertex model, a tag grammar (0/1/2/3/5/6/7/8/9/10/11), a
// stateful texture crop, a tag=9 tree and an animation-frame discriminant.
//
// Two conventions that bite every consumer:
//   - vertices are stored at 2x their true size -> multiply by kModelVertexScale
//   - local Y is stored INVERTED (-Y = up). The parser stays a faithful port and does NOT
//     correct it; every consumer of Vec3i::y negates at the point of use.
//
// Rewritten session 2026-09-13 on the confirmed multi-block model (h1/h2 are TOTALS across
// several vertex/face blocks, not one). The previous parser assumed a single block, guessed at
// a pre-face "mini-header" by trying every start offset and keeping whichever decoded the most
// faces, and papered over the resulting misreads with a resync mechanism and a vehicle-only
// is_face_degenerate() filter. All of that is gone: this parser is a single strict forward pass
// with NO start-offset search and NO resync - a record either matches a known shape at the
// current position or parsing stops and reports exactly where. Validated against every .3DW in
// the game's 20 vehicle archives (299 objects): 298/299 decode every face and end on the file's
// own {0,-1} terminator; the one exception is a single degenerate stub object (CABMINI.VHC's
// MSTWHEEL.3DW, h2=1) with a still-unexplained 11-int preamble, see the spec.
//
// NOT ported (deliberately, unchanged from before): the "second face-array block" LOD-variant
// merge (CACTUS.3DW style, believed to be genuinely separate content, not part of this model).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bigred {

// .3DW vertices are stored at 2x their true world size - CONFIRMED. Three independent
// geometric relationships in CHOPPER1's start gate snap into place at 0.5x and at no other
// scale. This also retroactively explains why .FT's
// bounding box looked "halved": .FT stored the true size all along.
constexpr float kModelVertexScale = 0.5f;

struct Vec3i {
    int32_t x = 0, y = 0, z = 0;
};

// One conditional branch of the compiled display list, as `{9, op, value, 7, distance}` (or the
// 6-int tag=10 form). CONFIRMED against the original's own branch routine, which compares
// the instance's variant byte with one of seven operators and then either falls through or takes
// the record's embedded tag=7 relative jump.
//
// ⚠️ The byte compared is the instance's variant byte. For most decor that is MAP.DAT offset-19, but
// on an ANIMATED collision category the engine rewrites a masked field of it every frame from
// the level script's `anim` line - which is how animated shapes change geometry. Same branches,
// different input. See level_script.h.
//
// ⚠️ NOT the [0x60, 0x7f] counter the same animation routine also writes: that is a second,
// separate mechanism in the same function, and no shipped instance meets its entry condition.
struct BranchMarker {
    int32_t tag = 9;     // 9 or 10
    int32_t pos = 0;     // int offset of this record in the stream
    int32_t op = 0;      // 0 = bitwise TEST, 1 = ==, 2 = >, 3 = <, 4 = >=, 5 = <=, 6 = !=
    int32_t value = 0;   // what the variant byte is compared against
    int32_t target = 0;  // int offset taken when the test is TRUE (via the embedded tag=7 jump)
    int32_t next = 0;    // int offset taken when it is FALSE (just past this record)
};

struct Face {
    std::optional<int32_t> color; // palette index, or nullopt for a logo-slot/textured face
    std::vector<int32_t> indices; // into ThreeDW::vertices
    // True for any tag=5 (TAG_TEXTURED) face WITH ITS OWN inline per-vertex UV - distinct from
    // a plain tag=6 logo-slot face (player-chosen vehicle logo, no variant list, still rendered
    // as a flat placeholder) or a tag=6 face that inherits a shape-level crop (see
    // ThreeDW::texture_variants and threedw.cpp's crop-inheritance tracking).
    bool textured_variant = false;
    // A tag=5 face's OWN inline per-vertex UVs (parallel to `indices`), in raw `.TM` texel
    // units. Only the LONG form of tag=5 (a real face record) carries these; the SHORT form
    // (see UVVariant) never does.
    std::vector<std::array<float, 2>> uv;
    int32_t tex_id = 0; // the LONG tag=5 form's own tex_id field.
                         // NOT the same thing as the short-form crop, which has no such field.
    // Which tag=9/tag=10-delimited group this face came from (0 = before any marker). A shape
    // can pack several groups after its base geometry; on FLAG/FLAG2 those are ANIMATION FRAMES
    // (the game shows one at a time) - see threedw.cpp's group tracking and main.cpp's use of it.
    int group = 0;
};

// {11, colour, v0, v1}: a LINE between two vertices - TELEPOLE's cross-arms, for one. The
// original draws it one pixel wide; see level_scene.cpp's build_shape_local_mesh for ours.
struct LineSeg {
    int32_t color = 0;
    int32_t v0 = 0, v1 = 0;
    int group = 0;            // as Face::group
    int32_t stream_pos = 0;   // same space as face_stream_pos
};

// One selectable texture crop for a face with no inline UV of its own - CONFIRMED session
// 2026-09-13 (re-derived byte-for-byte on JEEP1/IN_JEEP1/ROADSIGN/BILBORD2) to be EXACTLY `{5, X, count, U0,V0..U(count-1),V(count-1)}` - no
// trailing tex_id/extra field. The previous model's "tex_id" didn't exist: it was actually
// reading the leading tag of whatever record happened to follow (almost always another crop's
// `5`, a `{7,Y}`/`{8,Y}` marker, or the next real face's tag). `u0/v0/u1/v1` are the bounding
// rect of the UV quad, in raw texel units of the shape's single `embedded_texture_name` texture
// (same 256x192 space as terrain atlases - see texture.h). A shape can have many of these (13+
// in ROADSIGN.3DW, one per turn-arrow/message variant) - `MAP.DAT`'s raw offset-19 byte (see
// scenery.h) selects which one a given placed instance uses, 0-indexed in file order.
struct UVVariant {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    // The crop's OWN per-corner UV list, in file order - what the record actually contains. The
    // bounding rect above is derived from it.
    //
    // ⚠️ Keeping only the rect loses real information, and it cost a defect: ROADSIGN's crops 1
    // and 2 (and 3/4, 5/6, 8/9) are the SAME rectangle with the corners in a different order -
    // one arrow is the other mirrored. Reduced to a bounding box they became indistinguishable,
    // which is why SNOW1's seven directional signs all showed the same arrow. Emit from this
    // list, not from the rect, whenever it has one entry per face vertex.
    std::vector<std::array<float, 2>> uv;
    // The `.TM` this crop reads from, as a bare stem - the record NAMES ITS OWN SHEET and it is
    // not always the shape's first one. ROADSIGN's faces are on `ROADSIGN.TM` while its
    // destruction filmstrip is on `SIGN.TM`, and four shapes in the game do this
    // (ROADSIGN, MSIGN1, MSIGN2, CHPOST - all destructibles, all filmstrips).
    // Empty when the pointer did not resolve, in which case the shape's own sheet applies.
    std::string texture;
};

// A `{13, vertex, width, height}` record: a camera-facing SPRITE pinned to one of the shape's
// vertices, drawn with whatever texture crop the display list had current when it ran.
//
// CONFIRMED 2026-09-15 against the original's own routine for tag 13 (reached through the
// display list's routine table): it adds the operand to the transformed-vertex array base, reads that vertex's screen position and depth, and divides BOTH width and height by
// the depth before laying a rectangle around it. That is a billboard of constant WORLD size, not
// a screen-space icon. Cross-checked on the data: 69 of 69 such records across all 25 levels have
// a vertex index inside their own shape's array.
struct Sprite {
    int32_t vertex = 0;      // index into ThreeDW::vertices
    // HALF-extents, already in TRUE world units: unlike a vertex these do NOT carry the
    // .3DW's 2x storage scale, so kModelVertexScale must NOT be applied to them.
    int32_t width = 0, height = 0;
    int crop = -1;           // index into texture_variants, or -1 if none was current
    int32_t stream_pos = 0;  // same space as face_stream_pos, so the replay can gate it
};

struct ThreeDW; // fwd

// Every face group above 0 that actually contains faces, ascending (empty if none). A shape's
// tag=9/tag=10 markers form a TREE, not a flat run.
std::vector<int> frame_groups(const ThreeDW& model);

// RETIRED 2026-09-15, kept only because it is a useful measurement to re-run. It guessed which
// groups were animation frames from their geometry (congruent, same size, different position) -
// and it picked out exactly FLAG/FLAG2 across 12 levels, which looked like a success. It was
// answering the wrong question: a shape animates because the LEVEL SCRIPT rewrites its variant
// byte, not because its geometry looks repeated, and FLAG's 8 poses do not line up with its 4
// groups at all. Nothing in the renderer calls this any more.
std::vector<int> animation_frame_groups(const ThreeDW& model);

// What one replay of a shape's display list produced, for an instance with a given variant byte.
struct DisplayListReplay {
    std::vector<bool> drawn; // one per entry of ThreeDW::faces
    // Per face, which entry of ThreeDW::texture_variants was the CURRENT CROP when the walk
    // reached it, or -1 if none had run yet. A crop record is an instruction in the stream, not
    // a face: it sets the crop and every face after it inherits it until the next one.
    //
    // This is what "the branches select crops, not faces" means concretely - ROADSIGN reaches
    // the same faces for every variant and differs only here. Indexing the crop pool by the
    // variant byte, as we used to, was an approximation of this.
    std::vector<int> crop;
    // Per entry of ThreeDW::sprites: did the walk reach that record at all? A shape's sprites can
    // sit in a branch the default entry never takes - on most shapes they sit behind the
    // "destroyed" flag, bit 0x40 of the variant byte.
    std::vector<bool> sprite_drawn;
    // And which crop was current when it did. A sprite's crop is NOT fixed by file order: the
    // branches choose it, which is how one sprite record becomes an animation.
    std::vector<int> sprite_crop;
};

// Replays the display list's control flow for an instance whose variant byte is `variant`, and
// returns which faces it actually reaches - one bool per entry of `model.faces`.
//
// This is the ORIGINAL's own selection mechanism rather than an imitation of its results. A shape
// with no branches returns all true, which is the overwhelmingly common case.
std::vector<bool> faces_reached(const ThreeDW& model, int variant);

// Same replay from just the pieces that matter, so a caller can keep those instead of the whole
// parsed model (see LevelScene::ShapeBranches).
std::vector<bool> faces_reached_from(const std::vector<BranchMarker>& branches,
                                     const std::vector<int32_t>& face_stream_pos,
                                     size_t face_count, int variant,
                                     bool enter_at_prologue = false);

// The full replay, crop state included. `crop_stream_pos` may be empty, in which case every
// face reports crop -1 and this is exactly faces_reached_from.
// `enter_at_prologue` starts the walk at the display list's 2-int PROLOGUE instead of just
// after it. On a shape with a damaged body that prologue jumps straight to it, which is how a
// sheep is drawn flattened - see the walk's own comment.
DisplayListReplay replay_display_list(const std::vector<BranchMarker>& branches,
                                      const std::vector<int32_t>& face_stream_pos,
                                      const std::vector<int32_t>& crop_stream_pos,
                                      size_t face_count, int variant,
                                      const std::vector<int32_t>& sprite_stream_pos = {},
                                      bool enter_at_prologue = false);

struct ThreeDW {
    int32_t hitbox_radius = 0; // h0
    std::vector<Vec3i> vertices;
    std::vector<Face> faces;
    std::vector<LineSeg> lines;
    std::vector<UVVariant> texture_variants; // see UVVariant - empty for shapes with no
                                              // selectable texture (the common case)
    // Where each entry of `texture_variants` sits in the record stream, parallel to it and in the
    // same space as `face_stream_pos`. Needed to replay a crop as what it is - an instruction the
    // control flow either reaches or jumps over. See DisplayListReplay::crop.
    std::vector<int32_t> crop_stream_pos;
    // Every `{13, ...}` sprite the shape declares, in file order - see Sprite.
    std::vector<Sprite> sprites;
    // First readable ASCII run (>=3 chars, NUL-terminated) found in the file's own
    // name/material sub-block (offset 16, own length at offset 20) - the real `.TM` texture
    // reference for a `texture_variants`-bearing shape, which does NOT always match the shape's
    // own MAP.SHL name: `BILBORD2.3DW` embeds `"BIL2"` (-> `BIL2.TM`), `S_LIGHTS.3DW` embeds
    // `"GRASS"` (-> a shared/generic atlas, reused across unrelated shapes), `ROADSIGN.3DW`
    // embeds `"ROADSIGN"` (matches its own name here, but that's a coincidence, not the rule).
    // Empty if no such run was found.
    std::string embedded_texture_name;
    // How many vertices each successive vertex block declared, in file order (element 0 is the
    // block read from the fixed header offset; each further element came from a `{1, base,
    // count, ...}` record met mid-stream). Sums to `vertices.size()` (== h1 on a fully-decoded
    // object). Purely diagnostic.
    std::vector<int> vertex_block_sizes;
    // GROUND-CONFORM PROBES. Some shapes have a subset of their vertices re-projected onto the
    // terrain at draw time instead of being rigid - that is how a FENCE bends over a bump and
    // how a tunnel mouth's retaining walls stay flush with the hillside. This is the probe list
    // of the shape's NEAREST distance band: entry `i` is the XZ (and base Y) at which to sample
    // the heightmap for vertex `i`, so it is parallel to the first `size()` entries of
    // `vertices`. Empty when the shape has no such block, or when it fails the sanity checks -
    // the header fields these offsets live in mean something else entirely on shapes that are
    // never drawn through the conforming path.
    std::vector<Vec3i> ground_probes;
    // The CONDITIONAL BRANCHES of the original's display list, in file order. These are what
    // tag=9 / tag=10 markers really are - not "groups". DECODED and CONFIRMED 2026-09-14.
    // Positions are int32 offsets into the post-vertex record stream,
    // the same space as `face_stream_pos`.
    std::vector<BranchMarker> branches;
    // Where each entry of `faces` starts in that stream, parallel to `faces`. Needed to replay
    // the branches: a face is drawn only if control flow actually reaches its record.
    std::vector<int32_t> face_stream_pos;
    int parsed_face_count = 0; // may be < h2 for socket-composite vehicles (wheels etc in
                                // sibling .3DW files) - not a parse failure
    int expected_face_count = 0; // h2
    // True only if parsing reached the file's own {0,-1} end-of-stream terminator. False means
    // parsing stopped on an unrecognized record (a genuine vocabulary gap, see parse_stop_*) or
    // ran off the end of the data - `parsed_face_count` may still be meaningful in that case
    // (whatever was read before the stop), but nothing after the stop was read.
    bool reached_clean_end = false;
    // Diagnostics for a parse that did NOT reach a clean end: the byte offset (from the start of
    // the file) and the raw tag value that stumped the parser. Both 0 when reached_clean_end.
    size_t stop_byte_offset = 0;
    int32_t stop_tag = 0;
};

// Parses a raw .3DW file's bytes (as extracted from a .LEV/.VHC/.ENV archive via
// bigred_archive::unpack_file). Throws std::runtime_error only on a genuinely malformed file
// (bad header); a partial/blocked face-array parse is not an error, see parsed_face_count and
// reached_clean_end.
ThreeDW parse_3dw(const uint8_t* data, size_t size);

struct RGB {
    uint8_t r = 0, g = 0, b = 0;
};

// DEFAULT.COL is 768 raw bytes (256 * RGB), each component 0-63 (VGA 6-bit DAC) - scaled *4
// to 0-255 here, matching tools/visualize_3dw.py's load_palette_rgb.
std::vector<RGB> load_palette_rgb(const uint8_t* col_data, size_t size);

} // namespace bigred
