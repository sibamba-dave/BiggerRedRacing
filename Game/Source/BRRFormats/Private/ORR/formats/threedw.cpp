#include "threedw.h"

#include <limits>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace bigred {

namespace {

constexpr int32_t TAG_SECTION = 0;   // {0,-1} end / {0,1,base,count} new vertex block / bare {0}
constexpr int32_t TAG_VERTADD = 1;   // {1,base,count,count*(x,y,z)} - ADDS vertices
constexpr int32_t TAG_FLAT = 2;      // {2,count,colour,facenum,v0..} - flat-coloured face
constexpr int32_t TAG_MARKER3 = 3;   // {3,count,count*value} - unconfirmed meaning
constexpr int32_t TAG_TEXTURED = 5;  // long form (real face) or short form (crop selector)
constexpr int32_t TAG_LOGO = 6;      // {6,count,facenum,v0..} - no colour of its own
constexpr int32_t TAG_MARKER7 = 7;   // {7,Y} - bare 2-int marker, unconfirmed meaning
constexpr int32_t TAG_MARKER8 = 8;   // {8,Y} - bare 2-int marker, sibling of tag=7
constexpr int32_t TAG_GROUP = 9;     // {9,group,N,anchor(7|8),M} - face-group tree marker
// {10,vA,vB,vC,anchor(7|8),M} - NOT a sibling of tag=9 and NOT a variant test: a distance +
// screen-winding gate on three of the shape's own vertices. See the replay in
// replay_display_list.
constexpr int32_t TAG_GROUP10 = 10;
constexpr int32_t TAG_LINE = 11;     // {11,colour,v0,v1} - a line segment, not a face
// {14,count,colour,facenum,v0..} - a flat-coloured face, byte for byte the tag=2 grammar under a
// different tag. DECODED 2026-09-15 on CACTUS.3DW, whose first two faces are
// `{14,5,46,0,4,0,1,2,3}` and `{14,5,46,1,0,4,5,6,7}`: the counts, the sequential face numbers
// and the vertex indices all check out against the 8 vertices declared before them. Until this
// was read, CACTUS/CACTUS2/MARSPLAT/MARSPLA3 stopped at their very first face and rendered
// NOTHING.
constexpr int32_t TAG_FLAT14 = 14;

int32_t rd(const std::vector<int32_t>& vals, size_t idx) { return vals[idx]; }

// One attempt at reading a record at `vals[pos]`. On success, advances `*pos` and returns true;
// on failure, leaves `*pos` untouched and returns false. Kept as small, independently-testable
// functions rather than one monolithic dispatch, mirroring how each shape was verified
// separately against real files.
struct Parser {
    const std::vector<int32_t>& vals;
    size_t n;
    size_t running_vcount; // grows as vertex-add records are consumed
    // The whole file, and the byte offset `vals[0]` sits at. A crop's `X` field is a pointer
    // RELATIVE TO THE RECORD'S OWN ADDRESS, and it points back into the header's material
    // sub-block, so resolving it needs the raw bytes and the record's true file offset.
    const uint8_t* raw = nullptr;
    size_t raw_size = 0;
    size_t raw_base = 0;
    size_t name_block_end = 0; // 16 + namelen: a resolved pointer must land inside this

    // Reads the `.TM` stem a crop record at `vals[p]` names. `X` (the field at `p+1`) is added
    // to the record's own byte address and lands on a material sub-record laid out exactly like
    // the one the embedded-name scan looks for: `[u32 size][4 zero bytes][ASCII name][NUL]`.
    //
    // CONFIRMED on the whole game: of the 2 924 crop records in every `.LEV`/`.ENV`/`.VHC`/
    // `.WAP`, 2 923 resolve inside the sub-block to a clean stem, and 2 913 of those name a
    // `.TM` that really is in the same archive.
    std::string crop_texture(size_t p) const {
        if (!raw || p + 1 >= n) return {};
        const long long here = static_cast<long long>(raw_base + p * 4);
        const long long t = here + rd(vals, p + 1);
        if (t < 16 || static_cast<unsigned long long>(t) + 9 > name_block_end) return {};
        const size_t off = static_cast<size_t>(t);
        int32_t size_field;
        std::memcpy(&size_field, raw + off, 4);
        if (size_field < 1 || size_field > 64) return {};
        if (raw[off + 4] || raw[off + 5] || raw[off + 6] || raw[off + 7]) return {};
        size_t b = off + 8, e = b;
        while (e < name_block_end && raw[e] != 0) ++e;
        if (e == b || e >= name_block_end || e - b > 12) return {};
        for (size_t i = b; i < e; ++i) {
            const uint8_t c = raw[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (!ok) return {};
        }
        return std::string(reinterpret_cast<const char*>(raw + b), e - b);
    }

    // tag=1: {1, base, count, count*(x,y,z)} - ADDS vertices. CONFIRMED on WHEEL.3DW
    // ({1,24,4,...} declares vertices 24-27, and 24+4=28=h1) and re-derived on every
    // multi-block vehicle body (JEEP1's {0,1,102,62} boundary + this record's own inline data).
    // Only accepted when `base` matches the running vertex count - the anchor that makes this
    // safe to accept without any surrounding context.
    bool vertex_add(size_t* pos, std::vector<Vec3i>* out_vertices) const {
        size_t p = *pos;
        if (p + 3 > n || rd(vals, p) != TAG_VERTADD) return false;
        int32_t base = rd(vals, p + 1), count = rd(vals, p + 2);
        if (count < 0 || count > 256) return false;
        size_t need = 3 + static_cast<size_t>(count) * 3;
        if (p + need > n || static_cast<size_t>(base) != running_vcount) return false;
        for (int32_t i = 0; i < count; ++i) {
            out_vertices->push_back(Vec3i{rd(vals, p + 3 + 3 * i), rd(vals, p + 3 + 3 * i + 1),
                                           rd(vals, p + 3 + 3 * i + 2)});
        }
        *pos = p + need;
        return true;
    }

    // Branch markers: {9,op,value,anchor,M} - 5 ints - and {10,vA,vB,vC,anchor,M} - 6 ints -
    // anchor in {7,8} at a fixed position.
    //
    // ⚠️ The two are NOT the same record with an extra field, though the layout makes them look
    // like it. tag=9 compares the instance's variant byte; tag=10 reads nothing from the instance
    // at all - its three fields are VERTEX INDICES and it gates on distance and screen winding.
    // Decoding tag=10's first two fields as an operator and a value is what blanked the CHEVRONS
    // boards on DIRT1/DIRT3 - see replay_display_list.
    // Fills `out` with the decoded branch, when this is one. The embedded tag=7 is a RELATIVE
    // JUMP - the original advances by operand + 4 bytes from the tag=7 field's own address - so the
    // target is (anchor field) + 1 + distance/4 in int units.
    bool group_marker(size_t* pos, BranchMarker* out = nullptr) const {
        size_t p = *pos;
        if (p + 5 <= n && rd(vals, p) == TAG_GROUP && (rd(vals, p + 3) == 7 || rd(vals, p + 3) == 8)) {
            if (out) {
                out->tag = TAG_GROUP;
                out->pos = static_cast<int32_t>(p);
                out->op = rd(vals, p + 1);
                out->value = rd(vals, p + 2);
                out->target = static_cast<int32_t>(p + 3) + 1 + rd(vals, p + 4) / 4;
                out->next = static_cast<int32_t>(p + 5);
            }
            *pos = p + 5;
            return true;
        }
        if (p + 6 <= n && rd(vals, p) == TAG_GROUP10 && (rd(vals, p + 4) == 7 || rd(vals, p + 4) == 8)) {
            if (out) {
                out->tag = TAG_GROUP10;
                out->pos = static_cast<int32_t>(p);
                out->op = rd(vals, p + 1);
                out->value = rd(vals, p + 2);
                out->target = static_cast<int32_t>(p + 4) + 1 + rd(vals, p + 5) / 4;
                out->next = static_cast<int32_t>(p + 6);
            }
            *pos = p + 6;
            return true;
        }
        return false;
    }

    // A standalone {7, distance} is an UNCONDITIONAL JUMP and {8, distance} a CALL - they are
    // not inert markers. CONFIRMED in the original: tag=7 advances by operand + 4 bytes and
    // jumps; tag=8 does the same but first pushes the address of the next record (a return
    // address), and a lone tag=0 pops it - the matching return. Threaded code, all of it.
    bool marker7or8(size_t* pos, BranchMarker* out = nullptr) const {
        size_t p = *pos;
        if (p + 2 <= n && (rd(vals, p) == TAG_MARKER7 || rd(vals, p) == TAG_MARKER8)) {
            if (out) {
                out->tag = rd(vals, p);
                out->pos = static_cast<int32_t>(p);
                out->op = -1; // unconditional
                out->value = 0;
                out->target = static_cast<int32_t>(p) + 1 + rd(vals, p + 1) / 4;
                out->next = static_cast<int32_t>(p + 2);
            }
            *pos = p + 2;
            return true;
        }
        return false;
    }

    // {3, count, count*value} - CONFIRMED shape via IN_JEEP1-6.3DW's {3,4,43,43,47,47}, sitting
    // cleanly between two ordinary faces with no other interpretation fitting. Meaning of the
    // values is still open (the spec guesses palette indices).
    bool marker3(size_t* pos) const {
        size_t p = *pos;
        if (p + 2 > n || rd(vals, p) != TAG_MARKER3) return false;
        int32_t count = rd(vals, p + 1);
        if (count < 1 || count > 16 || p + 2 + static_cast<size_t>(count) > n) return false;
        *pos = p + 2 + static_cast<size_t>(count);
        return true;
    }

    // tag=12 glyph-quad record: {12, X, c0..c7, 13, F1, F2, F3, 0} - 15 ints, one per rendered
    // character. CONFIRMED via ROADSIGN.3DW: 7 of these back-to-back right after the shape's
    // texture-crop list and before its one selectable-texture face (face 6) - re-verified
    // session 2026-09-13 byte-for-byte (X=-1652,-1712,-1772... stepping by -60; corners
    // (1,1)(62,1)(62,86)(1,86) etc, trailer always `13,16,90,150,0`). Not reachable without
    // this: parsing used to stop dead here, silently losing the sign's own textured face and
    // everything after it. Meaning of the corner/
    // trailer fields beyond their fixed positions is not decoded - this is a skip, not a font
    // renderer.
    // tag=12, PLAIN form: {12, X, u0,v0, u1,v1, u2,v2, u3,v3} - 10 ints, a TEXTURE CROP under a
    // different tag. DECODED 2026-09-15 on SPIRE.3DW, which carries three of them interleaved
    // with `{7, N}` jumps and a `{9, 0, 1, 7, 52}` conditional:
    //     {12, -1964, (88,96),(135,96),(135,143),(88,143)}
    //     {12, -2032, (88,48),(135,48),(135,95), (88,95)}
    //     {12, -2080, (88,0), (135,0), (135,47), (88,47)}
    // Three stacked rectangles in one texture column - the frames of the warning light that
    // MOON1's script calls `;warning lights on spire`.
    //
    // ⚠️ This REPLACES two hardcoded tag=12 shapes ("text glyph", 15 ints, and "tube", 22 ints).
    // Both were this crop plus tag=13 trailers - one for the glyph, three for the tube - and
    // matching them as one unit SWALLOWED THEIR SPRITES. Decoding the pieces separately consumes
    // exactly the same bytes, keeps the same parse rate, and yields the sprites as well.
    // `X` is the same unexplained field the short tag=5 crop has.
    bool tag12_crop(size_t* pos, UVVariant* out) const {
        size_t p = *pos;
        if (p + 10 > n || rd(vals, p) != 12) return false;
        std::vector<std::array<float, 2>> uv;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        for (int i = 0; i < 4; ++i) {
            int32_t u = rd(vals, p + 2 + 2 * i), v = rd(vals, p + 2 + 2 * i + 1);
            // Texel coordinates of a .TM (256x192). Anything outside is not a crop.
            if (u < 0 || u > 255 || v < 0 || v > 191) return false;
            const float fu = static_cast<float>(u), fv = static_cast<float>(v);
            if (i == 0) { u0 = u1 = fu; v0 = v1 = fv; }
            else { u0 = std::min(u0, fu); v0 = std::min(v0, fv); u1 = std::max(u1, fu); v1 = std::max(v1, fv); }
            uv.push_back({fu, fv});
        }
        if (u0 == u1 || v0 == v1) return false; // a degenerate rect is not a crop
        *out = UVVariant{u0, v0, u1, v1, std::move(uv), crop_texture(p)};
        *pos = p + 10;
        return true;
    }

    // {13, Y, A, B} - 4 ints, meaning UNKNOWN, always following a tag=12 crop. The two tag=12
    // shapes recognised before this (glyph, tube) were really "a crop plus one or three of these":
    // the tube's `13, Y, 50, 50` triples and the glyph's single trailer are the same record, and
    // SPIRE ends with two of them carrying `100, 100` instead. Recognising it on its own is what
    // lets SPIRE/VSPIRE/MARSMARK reach their terminator. A and B look like a size or a scale;
    // nothing in the renderer reads them yet.
    bool tag13_trailer(size_t* pos, Sprite* out) const {
        size_t p = *pos;
        if (p + 4 > n || rd(vals, p) != 13) return false;
        const int32_t vtx = rd(vals, p + 1);
        if (vtx < 0 || static_cast<size_t>(vtx) >= running_vcount) return false;
        out->vertex = vtx;
        out->width = rd(vals, p + 2);
        out->height = rd(vals, p + 3);
        *pos = p + 4;
        return true;
    }

    bool line(size_t* pos, LineSeg* out) const {
        size_t p = *pos;
        if (p + 4 > n || rd(vals, p) != TAG_LINE) return false;
        int32_t v0 = rd(vals, p + 2), v1 = rd(vals, p + 3);
        if (v0 < 0 || static_cast<size_t>(v0) >= running_vcount) return false;
        if (v1 < 0 || static_cast<size_t>(v1) >= running_vcount) return false;
        *out = LineSeg{rd(vals, p + 1), v0, v1, 0, 0};
        *pos = p + 4;
        return true;
    }

    // tag=5 SHORT form: {5, X, count, U0,V0..U(count-1),V(count-1)} - EXACTLY 3+count*2 ints,
    // no trailing field. Re-derived byte-for-byte session 2026-09-13 (see UVVariant in
    // threedw.h): the previously-assumed trailing tex_id/extra never existed.
    bool short_crop(size_t* pos, UVVariant* out) const {
        size_t p = *pos;
        if (p + 3 > n || rd(vals, p) != TAG_TEXTURED) return false;
        int32_t count = rd(vals, p + 2);
        if (count < 2 || count > 16) return false;
        size_t need = 3 + static_cast<size_t>(count) * 2;
        if (p + need > n) return false;
        size_t uv_start = p + 3;
        float u0 = static_cast<float>(rd(vals, uv_start)), v0 = static_cast<float>(rd(vals, uv_start + 1));
        float u1 = u0, v1 = v0;
        std::vector<std::array<float, 2>> uv;
        uv.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            float u = static_cast<float>(rd(vals, uv_start + 2 * static_cast<size_t>(i)));
            float v = static_cast<float>(rd(vals, uv_start + 2 * static_cast<size_t>(i) + 1));
            uv.push_back({u, v});
            u0 = std::min(u0, u); v0 = std::min(v0, v);
            u1 = std::max(u1, u); v1 = std::max(v1, v);
        }
        *out = UVVariant{u0, v0, u1, v1, std::move(uv), crop_texture(p)};
        *pos = p + need;
        return true;
    }

    // A real face record: tag 2 (flat), 6 (logo/inherits-crop), or 5 LONG form (own inline UV -
    // {5,X,count,U0,V0..,tex_id,echo_count,facenum,v0..}, unchanged from before - not exercised
    // by any vehicle body, still needed for decor like BR_MID/BR_END's per-face texture slice).
    // `expected` is the strictly sequential next face number - every vehicle body in the game
    // numbers its faces 0,1,2,... with no gaps, block boundaries included, so this alone is
    // enough to reject a false match without any start-offset search or resync.
    bool face_record(size_t* pos, int32_t expected, Face* out) const {
        size_t p = *pos;
        if (p >= n) return false;
        int32_t a = rd(vals, p);
        if (a == TAG_FLAT || a == TAG_FLAT14) {
            if (p + 2 > n) return false;
            int32_t count = rd(vals, p + 1);
            if (count < 2 || count > 32) return false;
            size_t need = 4 + static_cast<size_t>(count);
            if (p + need > n) return false;
            int32_t color = rd(vals, p + 2), facenum = rd(vals, p + 3);
            if (facenum != expected) return false;
            std::vector<int32_t> idxs(count);
            for (int32_t i = 0; i < count; ++i) idxs[static_cast<size_t>(i)] = rd(vals, p + 4 + i);
            for (int32_t v : idxs) if (v < 0 || static_cast<size_t>(v) >= running_vcount) return false;
            out->color = color;
            out->indices = std::move(idxs);
            *pos = p + need;
            return true;
        }
        if (a == TAG_LOGO) {
            if (p + 2 > n) return false;
            int32_t count = rd(vals, p + 1);
            if (count < 2 || count > 32) return false;
            size_t need = 3 + static_cast<size_t>(count);
            if (p + need > n) return false;
            int32_t facenum = rd(vals, p + 2);
            if (facenum != expected) return false;
            std::vector<int32_t> idxs(count);
            for (int32_t i = 0; i < count; ++i) idxs[static_cast<size_t>(i)] = rd(vals, p + 3 + i);
            for (int32_t v : idxs) if (v < 0 || static_cast<size_t>(v) >= running_vcount) return false;
            out->color = std::nullopt;
            out->indices = std::move(idxs);
            *pos = p + need;
            return true;
        }
        if (a == TAG_TEXTURED) {
            if (p + 3 > n) return false;
            int32_t count = rd(vals, p + 2);
            if (count < 2 || count > 32) return false;
            size_t need = 6 + static_cast<size_t>(count) * 3;
            if (p + need > n) return false;
            size_t uv_start = p + 3;
            int32_t facenum = rd(vals, uv_start + static_cast<size_t>(count) * 2 + 2);
            if (facenum != expected) return false;
            size_t idx_start = uv_start + static_cast<size_t>(count) * 2 + 3;
            std::vector<int32_t> idxs(count);
            for (int32_t i = 0; i < count; ++i) idxs[static_cast<size_t>(i)] = rd(vals, idx_start + i);
            for (int32_t v : idxs) if (v < 0 || static_cast<size_t>(v) >= running_vcount) return false;
            out->color = std::nullopt;
            out->indices = std::move(idxs);
            out->textured_variant = true;
            out->uv.reserve(static_cast<size_t>(count));
            for (int32_t i = 0; i < count; ++i) {
                out->uv.push_back({static_cast<float>(rd(vals, uv_start + 2 * static_cast<size_t>(i))),
                                    static_cast<float>(rd(vals, uv_start + 2 * static_cast<size_t>(i) + 1))});
            }
            out->tex_id = rd(vals, uv_start + static_cast<size_t>(count) * 2);
            *pos = p + need;
            return true;
        }
        return false;
    }
};

// Speculative, side-effect-free lookahead used only to disambiguate tag=5's short vs long form
// (see the TAG_TEXTURED handling in parse_3dw) - deliberately narrower than the main loop's own
// fallback vocabulary: it does NOT accept the generic bare-zero boundary marker (matches
// literally any `0`, so it would validate almost anything and defeat the disambiguation), only
// things that are independently, structurally recognizable at `p`.
bool looks_like_record_start(const std::vector<int32_t>& vals, size_t n, const Parser& parser,
                              size_t p, int32_t expected_face) {
    if (p >= n) return false;
    if (p + 2 <= n && vals[p] == TAG_SECTION && vals[p + 1] == -1) return true; // {0,-1} end
    {
        size_t q = p;
        std::vector<Vec3i> tmp;
        if (parser.vertex_add(&q, &tmp)) return true;
    }
    {
        size_t q = p;
        if (parser.group_marker(&q)) return true;
    }
    {
        size_t q = p;
        if (parser.marker7or8(&q)) return true;
    }
    {
        size_t q = p;
        LineSeg tmp;
        if (parser.line(&q, &tmp)) return true;
    }
    {
        size_t q = p;
        Face tmp;
        if (parser.face_record(&q, expected_face, &tmp)) return true;
    }
    {
        size_t q = p;
        UVVariant tmp;
        if (parser.short_crop(&q, &tmp)) return true;
    }
    {
        size_t q = p;
        if (parser.marker3(&q)) return true;
    }
    return false;
}

} // namespace

ThreeDW parse_3dw(const uint8_t* data, size_t size) {
    if (size < 24) throw std::runtime_error(".3DW file too small for header");

    int32_t h0, h1, h2, h3;
    std::memcpy(&h0, data + 0, 4);
    std::memcpy(&h1, data + 4, 4);
    std::memcpy(&h2, data + 8, 4);
    std::memcpy(&h3, data + 12, 4);
    (void)h3;
    int32_t namelen;
    std::memcpy(&namelen, data + 20, 4);
    if (namelen < 0 || static_cast<size_t>(namelen) + 56 > size) {
        throw std::runtime_error(".3DW name block length exceeds file size");
    }

    ThreeDW out;
    out.hitbox_radius = h0;
    out.expected_face_count = h2;

    // Scan the name/material sub-block (offset 16, `namelen` bytes) for the first
    // `[small u32 length][4 zero bytes][ASCII name][NUL]` sub-record - see
    // ThreeDW::embedded_texture_name. A plain "first readable ASCII run" scan isn't reliable
    // here - short binary fields elsewhere in the block coincidentally fall in the printable
    // range too (e.g. bytes `20 4e 4c` read as the text " NL") - this specific length-prefixed
    // shape is what actually and reliably identifies a real embedded name.
    {
        size_t block_start = 16, block_end = std::min<size_t>(16 + static_cast<size_t>(namelen), size);
        for (size_t i = block_start; i + 9 <= block_end; ++i) {
            int32_t len_field;
            std::memcpy(&len_field, data + i, 4);
            if (len_field < 1 || len_field > 64) continue;
            bool zero4 = data[i + 4] == 0 && data[i + 5] == 0 && data[i + 6] == 0 && data[i + 7] == 0;
            if (!zero4) continue;
            size_t name_start = i + 8;
            if (name_start >= block_end || data[name_start] < 'A' || data[name_start] > 'Z') continue;
            size_t j = name_start;
            while (j < block_end && data[j] != 0) ++j;
            if (j == name_start || j >= block_end) continue;
            out.embedded_texture_name.assign(reinterpret_cast<const char*>(data + name_start), j - name_start);
            break;
        }
    }

    // The 10 previously-undecoded int32s between the name block and the vertex array. Index 9
    // is the vertex count of the FIRST block - CONFIRMED session 2026-09-13 (h1/h2 are TOTALS,
    // not one block's count).
    size_t mid_start = 16 + static_cast<size_t>(namelen);
    if (mid_start + 40 > size) throw std::runtime_error(".3DW pre-vertex header exceeds file size");
    int32_t mid[10];
    std::memcpy(mid, data + mid_start, 40);
    // mid[7] sits right after h2 and is 1 on every shape in the game BUT FIVE - SHEEP, BILBOARD,
    // PLANT, PLANT2 and W_PLANT, where it is 9. DECODED 2026-09-15: it says WHERE the first
    // vertex block is. At 1 the header declares it, and mid[9] is its count (the long-standing
    // reading). At 9 the header declares none: the record stream starts immediately and brings
    // its own `{1, base, count}` block, after whatever display-list records come first - SHEEP
    // opens with a `{7, 2660}` jump then `{1, 0, 68}`, W_PLANT with `{7, 1384}`, a `{9, ...}`
    // conditional, then `{1, 0, 28}`. In both cases face 0 then lands exactly where the block
    // ends, to the int.
    //
    // Reading mid[9] as a count anyway is what made these five shapes decode ZERO faces and
    // vanish from the game: it claimed 64 vertices for SHEEP (the real block has 68) and 1 for
    // the plants, so the stream was entered mid-record.
    const bool header_declares_first_block = (mid[7] != 9);
    int32_t bc1 = header_declares_first_block ? mid[9] : 0;
    if (bc1 < 0 || bc1 > h1) throw std::runtime_error(".3DW first block's vertex count is invalid");

    size_t vstart = static_cast<size_t>(namelen) + 56;
    size_t vend = vstart + static_cast<size_t>(bc1) * 12;
    if (vend > size) throw std::runtime_error(".3DW vertex block exceeds file size");

    out.vertices.reserve(static_cast<size_t>(h1));
    for (int32_t i = 0; i < bc1; ++i) {
        int32_t x, y, z;
        std::memcpy(&x, data + vstart + i * 12 + 0, 4);
        std::memcpy(&y, data + vstart + i * 12 + 4, 4);
        std::memcpy(&z, data + vstart + i * 12 + 8, 4);
        out.vertices.push_back(Vec3i{x, y, z});
    }
    if (bc1 > 0) out.vertex_block_sizes.push_back(bc1);

    // Ground-conform probe block - see ThreeDW::ground_probes. The header carries three
    // distance bands at a stride of 0x10 (threshold / destination / source); only the first,
    // nearest band matters here since this renderer draws every shape at full detail.
    //   threshold = u16 @0x22,  dest = 0x24 + i32@0x24,  src = 0x2c + i32@0x2c
    // Both are SELF-RELATIVE offsets, and each block holds a byte count at +0x10 and a vertex
    // array at +0x14 (stride 12) - the same shape as the main vertex block.
    if (size >= 0x30) {
        int32_t dest_rel = 0, src_rel = 0;
        std::memcpy(&dest_rel, data + 0x24, 4);
        std::memcpy(&src_rel, data + 0x2c, 4);
        int64_t dest = 0x24 + static_cast<int64_t>(dest_rel);
        int64_t src = 0x2c + static_cast<int64_t>(src_rel);
        if (dest >= 0 && src >= 0 && static_cast<size_t>(dest) + 0x14 <= size &&
            static_cast<size_t>(src) + 0x14 <= size) {
            size_t count = data[static_cast<size_t>(src) + 0x10];
            // Three guards, all needed. The band fields overlap bytes that carry unrelated data
            // on shapes never drawn through the conforming path (CHTREE, S_GRID), and those
            // decode to plausible-looking but nonsensical blocks: CHTREE claims 255 probes for
            // 65 vertices. Requiring the destination to BE the main vertex array, and the count
            // to fit inside it, rejects every such case on the levels checked.
            const bool dest_is_main_block =
                static_cast<size_t>(dest) + 0x14 == vstart &&
                data[static_cast<size_t>(dest) + 0x10] == static_cast<uint8_t>(bc1 & 0xff);
            if (count > 0 && count <= static_cast<size_t>(bc1) && dest_is_main_block &&
                static_cast<size_t>(src) + 0x14 + count * 12 <= size) {
                out.ground_probes.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    int32_t px, py, pz;
                    size_t o = static_cast<size_t>(src) + 0x14 + i * 12;
                    std::memcpy(&px, data + o + 0, 4);
                    std::memcpy(&py, data + o + 4, 4);
                    std::memcpy(&pz, data + o + 8, 4);
                    out.ground_probes.push_back(Vec3i{px, py, pz});
                }
            }
        }
    }

    size_t post_bytes = size - vend;
    size_t n = post_bytes / 4;
    std::vector<int32_t> vals(n);
    for (size_t i = 0; i < n; ++i) std::memcpy(&vals[i], data + vend + i * 4, 4);

    Parser parser{vals, n, static_cast<size_t>(bc1), data, size, vend,
                  std::min<size_t>(16 + static_cast<size_t>(namelen), size)};
    size_t pos = 0;
    int current_group = 0;
    // The texture crop is STATEFUL across records - a tag=5 crop (long or short form) sets the
    // current crop and every following colourless (tag=6) face with no UV of its own inherits it
    // until the next tag=5, as BR_END.3DW shows.
    std::vector<std::array<float, 2>> last_uv;

    // Where the record that produced each face began, so the branches can be replayed later.
    // Backfilled at the top of each iteration for whatever the previous one emitted.
    size_t iter_pos = 0;
    auto record_face_positions = [&](size_t p) {
        while (out.face_stream_pos.size() < out.faces.size()) {
            out.face_stream_pos.push_back(static_cast<int32_t>(p));
        }
    };

    for (;;) {
        record_face_positions(iter_pos);
        if (pos >= n) break; // ran off the end without a clean terminator
        iter_pos = pos;

        // {0,-1} end-of-stream / {0,1,base,count} new vertex block / bare {0} boundary marker.
        if (rd(vals, pos) == TAG_SECTION) {
            if (pos + 2 <= n && rd(vals, pos + 1) == -1) {
                // ⚠️ `{0, -1}` is NOT a special two-int record at the machine level: the `0` slot
                // holds the SAME routine a lone zero does, i.e. RETURN, and the `-1` is just
                // whatever sits after it. Returning with an
                // empty stack is what ends the list - which is why this reads as a terminator at
                // top level. Record it as a return marker so a jump can LAND on it: BEACON's
                // variant-decoding subroutine has leaves that branch here, and treating it as an
                // unconditional stop swallowed the return, so the whole beacon vanished on two
                // steps of its cycle. That is the "buildings blinking" the user reported on MOON2.
                out.branches.push_back(BranchMarker{TAG_SECTION, static_cast<int32_t>(pos), -2, 0,
                                                    static_cast<int32_t>(pos),
                                                    static_cast<int32_t>(pos)});
                out.reached_clean_end = true;
                pos += 2;
                break;
            }
            // ⚠️ `{0, 1, base, count}` is NOT one record: it is a LONE ZERO followed by a bare
            // `{1, base, count}` block, exactly as the spec always said ("the 1 here is just the
            // normal tag=1 record - the 0 before it is not part of it"). The parser used to eat
            // the four ints together, which read the same vertices but SWALLOWED THE STOP, and
            // that is what left SHEEP flattened: its intact body ends at a lone zero, the
            // squashed body begins at the bare block right after it, and the file's own jump
            // targets that block to the int. Eating the zero ran the two bodies together.
            // Falling through here leaves the zero to the lone-stop marker below, and the bare
            // `{1, base, count}` handler in the main loop reads the block on the next pass.

            // A lone tag=0 ENDS the display list's walk - CONFIRMED 2026-09-14. In the compiled
            // threaded code every slot that holds a tag is replaced by a routine pointer, and
            // this one's routine stops. It is what terminates each branch body: S_GRID has one
            // after each of its four alternatives, and without it a replay of the branches runs
            // straight from one alternative into the next.
            out.branches.push_back(BranchMarker{TAG_SECTION, static_cast<int32_t>(pos), -2, 0,
                                                 static_cast<int32_t>(pos), static_cast<int32_t>(pos)});
            // Historical note on what it is NOT: a 1-int boundary marker of
            // unknown meaning - CONFIRMED session 2026-09-13 by hand-tracing ATV1/BEETLE1/
            // BUGGY1/IN_JET1/SNOWCAT1/FEHELI1: every occurrence is followed immediately by an
            // ordinary, sequentially-numbered face record (or, at end of file, by the real
            // `{0,-1}` terminator handled above). This is what used to look like a large,
            // varied vocabulary of `{0, kind, ...}` "section" records (kind=2 by far the most
            // common in the vehicle corpus) - kind was never a real discriminator, just
            // whatever tag the NEXT, wholly unrelated record happened to carry.
            ++pos;
            continue;
        }

        std::vector<Vec3i> added;
        if (parser.vertex_add(&pos, &added)) {
            out.vertices.insert(out.vertices.end(), added.begin(), added.end());
            out.vertex_block_sizes.push_back(static_cast<int>(added.size()));
            parser.running_vcount += added.size();
            continue;
        }

        BranchMarker marker;
        if (parser.group_marker(&pos, &marker)) {
            out.branches.push_back(marker);
            ++current_group;
            continue;
        }

        BranchMarker jump;
        if (parser.marker7or8(&pos, &jump)) {
            out.branches.push_back(jump);
            continue;
        }

        LineSeg seg;
        if (parser.line(&pos, &seg)) {
            seg.group = current_group;
            seg.stream_pos = static_cast<int32_t>(iter_pos);
            out.lines.push_back(seg);
            continue;
        }

        // tag=5 is ambiguous on its own: both the LONG form (a real face, own inline UV) and
        // the SHORT form (a crop selector, no face at all) start `{5, X, count, UV pairs...}`
        // - the short form just stops there while the long form keeps going with tex_id/
        // echo_count/facenum/indices. CONFIRMED session 2026-09-13 via BILBORD2.3DW: its 5th
        // crop (a real, valid 11-int short form) was being swallowed by the long-form branch
        // instead, because the bytes immediately following it (a `{7,Y}` marker, then the next
        // real face's own tag/count/facenum/indices) happen to line up such that the long
        // form's own derived `facenum` field lands exactly on that unrelated next face's
        // facenum, which is `expected` by sheer coincidence - a false accept with no vertex-
        // index range violation to catch it. Effect: that face wrongly got an inline UV (the
        // WRONG crop rectangle) instead of correctly falling back to the shape's `MAP.DAT`
        // offset-19-selected variant, permanently showing the wrong picture regardless of
        // which variant a given placed instance asked for.
        // Fix: prefer the SHORT interpretation, but ONLY once at least one short crop has
        // already been confirmed in this file (i.e. we are demonstrably inside a variant
        // "pool" already, per the doc's crop-count evidence) AND it is immediately followed by
        // something independently recognizable. CORRECTED session 2026-09-13 (same day, second
        // fix): an unconditional preference regressed EVERY shape that uses the long form for
        // real geometry - START, BR_END, BR_MID, FOOT_BR - because with zero prior crops the
        // short reading is offered no anchor at all and happily "validates" against whatever
        // the long form's own trailing fields coincidentally look like, silently turning every
        // one of their long-form faces into a bogus crop (checkpoint banners lost their real
        // per-design UV and showed a garbled composite of all four stacked strips - the
        // "German flag" effect - and the bridges lost their own per-face texture slices
        // entirely). None of those shapes ever has a real crop BEFORE their first long-form
        // face in file order, so gating on `texture_variants` already containing at least one
        // confirmed crop reproduces the exact fix for BILBORD2.3DW's 5th crop (found after 4
        // already-confirmed ones) without touching any of them.
        if (rd(vals, pos) == TAG_TEXTURED && !out.texture_variants.empty()) {
            size_t crop_pos = pos;
            UVVariant crop;
            if (parser.short_crop(&crop_pos, &crop) &&
                looks_like_record_start(vals, n, parser, crop_pos, static_cast<int32_t>(out.faces.size()))) {
                pos = crop_pos;
                out.texture_variants.push_back(crop);
                out.crop_stream_pos.push_back(static_cast<int32_t>(iter_pos));
                last_uv = {{{crop.u0, crop.v0}, {crop.u1, crop.v0}, {crop.u1, crop.v1}, {crop.u0, crop.v1}}};
                continue;
            }
        }

        // A vertex block also appears WITHOUT the leading zero, as a bare `{1, base, count}`.
        // CACTUS.3DW puts one right after its first two faces (`{1, 8, 42}` - 42 more vertices
        // from index 8, and 8 + 42 = 50 = h1). Guarded by `base == running_vcount` exactly like
        // the `{0, 1, ...}` form, so it cannot swallow an unrelated record that starts with 1.
        if (rd(vals, pos) == TAG_VERTADD && pos + 3 <= n &&
            static_cast<size_t>(rd(vals, pos + 1)) == parser.running_vcount) {
            int32_t count = rd(vals, pos + 2);
            if (count >= 0 && count <= 4096) {
                size_t need = 3 + static_cast<size_t>(count) * 3;
                if (pos + need <= n) {
                    for (int32_t i = 0; i < count; ++i) {
                        out.vertices.push_back(Vec3i{rd(vals, pos + 3 + 3 * i),
                                                      rd(vals, pos + 3 + 3 * i + 1),
                                                      rd(vals, pos + 3 + 3 * i + 2)});
                    }
                    out.vertex_block_sizes.push_back(count);
                    parser.running_vcount += static_cast<size_t>(count);
                    pos += need;
                    continue;
                }
            }
        }

        int32_t expected = static_cast<int32_t>(out.faces.size());
        Face face;
        if (parser.face_record(&pos, expected, &face)) {
            face.group = current_group;
            if (!face.uv.empty()) {
                last_uv = face.uv;
            } else if (!face.color.has_value() && !last_uv.empty() && last_uv.size() == face.indices.size() &&
                       out.texture_variants.size() <= 1) {
                // Only inherit the running crop when AT MOST ONE has been seen so far.
                // Re-checked session 2026-09-13 on the actual data:
                // BR_MID/FOOT_BR each carry exactly ONE short crop total, immediately followed
                // by many colourless faces that all genuinely share it (real evidence for
                // inheritance). BILBORD2/ROADSIGN instead front-load MANY crops (5 and 14) as a
                // SELECTABLE POOL for MAP.DAT's offset-19, consumed by NONE of the shape's own
                // faces directly - inheriting the last one anyway made BILBORD2's sign always
                // show whichever crop happened to be listed last (a rabbit joke image, picked
                // regardless of the per-instance offset-19 that should have chosen the correct
                // warning text) instead of falling through to the variant-list lookup that
                // was supposed to apply exactly because the face carries no UV of its own.
                face.uv = last_uv;
                face.textured_variant = true;
            }
            out.faces.push_back(std::move(face));
            continue;
        }

        UVVariant crop;
        if (parser.short_crop(&pos, &crop)) {
            out.texture_variants.push_back(crop);
            out.crop_stream_pos.push_back(static_cast<int32_t>(iter_pos));
            last_uv = {{{crop.u0, crop.v0}, {crop.u1, crop.v0}, {crop.u1, crop.v1}, {crop.u0, crop.v1}}};
            continue;
        }

        if (parser.marker3(&pos)) continue;

        Sprite sprite;
        if (parser.tag13_trailer(&pos, &sprite)) {
            sprite.crop = out.texture_variants.empty()
                              ? -1 : static_cast<int>(out.texture_variants.size()) - 1;
            sprite.stream_pos = static_cast<int32_t>(iter_pos);
            out.sprites.push_back(sprite);
            continue;
        }

        UVVariant crop12;
        if (parser.tag12_crop(&pos, &crop12)) {
            out.texture_variants.push_back(crop12);
            out.crop_stream_pos.push_back(static_cast<int32_t>(iter_pos));
            continue;
        }

        out.stop_byte_offset = vend + pos * 4;
        out.stop_tag = rd(vals, pos);
        break;
    }

    out.parsed_face_count = static_cast<int>(out.faces.size());
    return out;
}

std::vector<RGB> load_palette_rgb(const uint8_t* col_data, size_t size) {
    std::vector<RGB> palette;
    size_t count = size / 3;
    palette.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto scale = [](uint8_t b) -> uint8_t { return static_cast<uint8_t>(std::min(255, b * 4)); };
        palette.push_back(RGB{scale(col_data[i * 3 + 0]), scale(col_data[i * 3 + 1]), scale(col_data[i * 3 + 2])});
    }
    return palette;
}

std::vector<bool> faces_reached(const ThreeDW& model, int variant) {
    return faces_reached_from(model.branches, model.face_stream_pos, model.faces.size(), variant);
}

std::vector<bool> faces_reached_from(const std::vector<BranchMarker>& branches,
                                     const std::vector<int32_t>& face_stream_pos,
                                     size_t face_count, int variant,
                                     bool enter_at_prologue) {
    return replay_display_list(branches, face_stream_pos, {}, face_count, variant, {},
                               enter_at_prologue).drawn;
}

DisplayListReplay replay_display_list(const std::vector<BranchMarker>& branches,
                                      const std::vector<int32_t>& face_stream_pos,
                                      const std::vector<int32_t>& crop_stream_pos,
                                      size_t face_count, int variant,
                                      const std::vector<int32_t>& sprite_stream_pos,
                                      bool enter_at_prologue) {
    const std::vector<BranchMarker>& model_branches = branches;
    const std::vector<int32_t>& model_face_stream_pos = face_stream_pos;
    DisplayListReplay out_replay;
    std::vector<bool>& drawn = out_replay.drawn;
    drawn.assign(face_count, false);
    out_replay.crop.assign(face_count, -1);
    out_replay.sprite_drawn.assign(sprite_stream_pos.size(), false);
    out_replay.sprite_crop.assign(sprite_stream_pos.size(), -1);
    if (model_branches.empty() || model_face_stream_pos.size() != face_count) {
        out_replay.sprite_drawn.assign(sprite_stream_pos.size(), true);
        // No conditionals (or no position data): everything the parser found is drawn, which is
        // what the engine did before any of this existed. The crop still has to be tracked, since
        // a crop-then-faces run is exactly how a bridge deck gets its per-face slice.
        drawn.assign(face_count, true);
        for (size_t f = 0; f < face_count; ++f) {
            int last = -1;
            for (size_t c = 0; c < crop_stream_pos.size(); ++c) {
                if (f < model_face_stream_pos.size() && crop_stream_pos[c] <= model_face_stream_pos[f]) {
                    last = static_cast<int>(c);
                }
            }
            out_replay.crop[f] = last;
        }
        return out_replay;
    }

    // The byte the original compares is zero-extended from 8 bits (MOVZX), the operand is not.
    const int32_t v = static_cast<int32_t>(static_cast<uint8_t>(variant));
    auto condition_true = [&](const BranchMarker& b) {
        switch (b.op) {
            case 0: return (b.value & v) != 0; // TEST/JNZ
            case 1: return v == b.value;
            case 2: return v > b.value;
            case 3: return v < b.value;
            case 4: return v >= b.value;
            case 5: return v <= b.value;
            case 6: return v != b.value;
            default: return false; // an operator we have not seen: fall through rather than jump
        }
    };

    // Walk the stream: at each step take whichever comes first from here, a branch or a face.
    // Bounded by the number of records so a malformed jump cannot loop forever.
    //
    // ⚠️ tag=8 is a CALL and a lone tag=0 is its RETURN, not a global stop - in the original,
    // tag=0 pops the return address and jumps to it. Treating tag=8 as a plain jump made the
    // walk end at the subroutine's return and reach nothing at all, which is exactly how CITY4's
    // START banners vanished. A return with an empty stack does end the walk.
    std::vector<int32_t> return_stack;
    // THE DISPLAY LIST HAS A 2-INT PROLOGUE, and the default drawing starts AFTER it.
    //
    // 8 of the game's 205 decor shapes open with an unconditional `{7, N}` at position 0. On
    // PINE it targets position 2 - the very next record, a no-op - which is what gives the slot
    // away: it is a fixed prologue the compiler always emits. On a shape that has a DAMAGED
    // state it points at that state instead: SHEEP jumps to its flattened body, PLANT/PLANT2/
    // W_PLANT to their crushed one, and entering at 0 draws exactly that. Measured, entering at
    // 2 instead: SHEEP reaches faces 0-57 (the standing sheep, the other 15 being the flat one),
    // the plants reach 36 of 54, and every shape whose prologue is a no-op is unchanged. The
    // sheep rendering permanently squashed was this, reported by the user 2026-09-15.
    //
    // ⚠️ HYPOTHÈSE on the WHY: the original presumably enters at the prologue and has the
    // collision code redirect it, or enters past it from the shape's LOD table. We have not
    // pinned down how the entry offset is computed, so this is the measured rule, not the
    // original's mechanism.
    int32_t cursor = 0;
    if (!enter_at_prologue && !model_branches.empty() && model_branches[0].pos == 0 &&
        model_branches[0].tag == 7 && model_branches[0].op == -1) {
        cursor = 2;
    }
    int current_crop = -1;
    const size_t budget = model_branches.size() + face_count + crop_stream_pos.size() +
                          sprite_stream_pos.size() + 8;
    for (size_t step = 0; step < budget; ++step) {
        int32_t best = std::numeric_limits<int32_t>::max();
        int best_kind = -1; // 0 = branch, 1 = face
        size_t best_index = 0;
        for (size_t i = 0; i < model_branches.size(); ++i) {
            if (model_branches[i].pos >= cursor && model_branches[i].pos < best) {
                best = model_branches[i].pos; best_kind = 0; best_index = i;
            }
        }
        for (size_t i = 0; i < model_face_stream_pos.size(); ++i) {
            if (model_face_stream_pos[i] >= cursor && model_face_stream_pos[i] < best) {
                best = model_face_stream_pos[i]; best_kind = 1; best_index = i;
            }
        }
        for (size_t i = 0; i < crop_stream_pos.size(); ++i) {
            if (crop_stream_pos[i] >= cursor && crop_stream_pos[i] < best) {
                best = crop_stream_pos[i]; best_kind = 2; best_index = i;
            }
        }
        for (size_t i = 0; i < sprite_stream_pos.size(); ++i) {
            if (sprite_stream_pos[i] >= cursor && sprite_stream_pos[i] < best) {
                best = sprite_stream_pos[i]; best_kind = 3; best_index = i;
            }
        }
        if (best_kind == 3) {
            out_replay.sprite_drawn[best_index] = true;
            out_replay.sprite_crop[best_index] = current_crop;
            cursor = best + 1;
            continue;
        }
        if (best_kind < 0) break; // nothing left ahead
        if (best_kind == 2) {
            // A crop record is an INSTRUCTION, not a face: it sets the current crop and every
            // face the walk reaches afterwards uses it, until the next one. That is why a shape
            // like ROADSIGN branches to the same faces for every variant - the branches pick
            // which crop runs, not which faces do.
            current_crop = static_cast<int>(best_index);
            cursor = best + 1;
        } else if (best_kind == 0) {
            const BranchMarker& b = model_branches[best_index];
            if (b.op == -2) {                      // lone tag=0: RETURN
                if (return_stack.empty()) break;   // nothing to return to - the walk is over
                cursor = return_stack.back();
                return_stack.pop_back();
            } else if (b.op == -1) {               // unconditional
                if (b.tag == 8) return_stack.push_back(b.next); // CALL: remember where to resume
                cursor = b.target;
            } else if (b.tag == TAG_GROUP10) {
                // ⚠️ tag=10 IS NOT A VARIANT TEST. In the original it reads NOTHING from the
                // instance: its three operands are
                // VERTEX INDICES, rewritten to byte offsets into the transformed-vertex buffer at
                // load time exactly as the tag itself is rewritten to a routine pointer. It then
                //
                //   if (A.depth < 0x400000)                  -> fall through
                //   else if (cross2d(B-A, C-A) >= 0)         -> TAKE the jump
                //   else                                     -> fall through
                //
                // i.e. a combined DISTANCE + SCREEN-WINDING gate: it only ever branches away for
                // an object that is both FAR and turned away. A near object always falls through.
                // Since we bake one static mesh and a decor object is read close up, falling
                // through is the faithful choice - and it is what the data demands. On DIRT1 and
                // DIRT3, CHEVRONS carries `{10, 3, 2, 1, 7, 540}` (its board quad's own vertices)
                // ahead of the `{9, &1}` that picks left- or right-pointing chevrons. Read as a
                // variant test, "op=3 (<), value=2" sent every shipped instance - the variant byte
                // is only ever 0 or 1 on all 8 levels that place the shape - straight past the
                // chevrons, leaving a BLANK BOARD ON A WOODEN POST. That was the user's report of
                // 2026-09-16: "DIRT1 has blank signs, DIRT2 doesn't" - DIRT2 has no CHEVRONS at
                // all, and CHOPPER/SNOW/RUF carry the same shape 24 bytes shorter, WITHOUT this
                // record, which is why only the two DIRT circuits showed it.
                //
                // 111 shapes in the game carry a tag=10, vehicles included.
                cursor = b.next;
            } else {
                cursor = condition_true(b) ? b.target : b.next;
            }
        } else {
            drawn[best_index] = true;
            out_replay.crop[best_index] = current_crop;
            cursor = best + 1;
        }
    }
    return out_replay;
}

std::vector<int> frame_groups(const ThreeDW& model) {
    std::vector<int> out;
    for (const Face& f : model.faces) {
        if (f.group <= 0) continue;
        if (std::find(out.begin(), out.end(), f.group) == out.end()) out.push_back(f.group);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> animation_frame_groups(const ThreeDW& model) {
    std::vector<int> groups = frame_groups(model);
    if (groups.size() < 2) return {};

    struct Stats {
        size_t faces = 0;
        std::vector<int32_t> verts; // distinct vertex indices used
        float size[3] = {0, 0, 0};  // bounding-box extents
        float min[3] = {0, 0, 0};   // bounding-box minimum corner
    };
    std::vector<Stats> stats(groups.size());
    for (const Face& f : model.faces) {
        auto git = std::find(groups.begin(), groups.end(), f.group);
        if (git == groups.end()) continue;
        Stats& st = stats[static_cast<size_t>(git - groups.begin())];
        ++st.faces;
        for (int32_t i : f.indices) {
            if (std::find(st.verts.begin(), st.verts.end(), i) == st.verts.end()) st.verts.push_back(i);
        }
    }
    for (size_t g = 0; g < groups.size(); ++g) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int32_t i : stats[g].verts) {
            if (i < 0 || static_cast<size_t>(i) >= model.vertices.size()) continue;
            const Vec3i& v = model.vertices[static_cast<size_t>(i)];
            const float c[3] = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
            for (int k = 0; k < 3; ++k) {
                if (c[k] < lo[k]) lo[k] = c[k];
                if (c[k] > hi[k]) hi[k] = c[k];
            }
        }
        for (int k = 0; k < 3; ++k) {
            stats[g].size[k] = hi[k] - lo[k];
            stats[g].min[k] = lo[k];
        }
    }
    bool moves = false;
    for (size_t g = 1; g < groups.size(); ++g) {
        if (stats[g].faces != stats[0].faces) return {};
        if (stats[g].verts.size() != stats[0].verts.size()) return {};
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(stats[g].size[k] - stats[0].size[k]) > 1.0f) return {};
            if (std::fabs(stats[g].min[k] - stats[0].min[k]) > 1.0f) moves = true;
        }
    }
    // Congruent but sitting in exactly the same place is a re-skin, not motion - MOON1's
    // BRIDGEND is two 2-face groups with an identical bounding box and different palette
    // indices (41/44 vs 38/41), i.e. the same quads in two shades. Blinking between those would
    // be wrong; a flag that flaps genuinely moves (FLAG's two 48-face cloths are mirrored in X,
    // x[-156..78] vs x[-78..156]).
    if (!moves) return {};
    return groups;
}

} // namespace bigred
