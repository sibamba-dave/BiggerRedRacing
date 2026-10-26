#include "vehicle_script.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace bigred {
namespace {

// The script tokenizer's own number syntax: decimal, or hex prefixed `&`, optionally negated.
// (The original's signed-char separator quirk only matters for bytes >= 0x80, which no vehicle
// script contains.)
bool parse_num(const std::string& tok, int32_t& out) {
    if (tok.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (tok[i] == '-') { neg = true; ++i; }
    if (i >= tok.size()) return false;
    int64_t v = 0;
    if (tok[i] == '&') {
        ++i;
        if (i >= tok.size()) return false;
        v = static_cast<int64_t>(std::strtoull(tok.c_str() + i, nullptr, 16));
    } else {
        if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return false;
        v = std::strtoll(tok.c_str() + i, nullptr, 10);
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One source line, comment stripped, split on whitespace and commas (the tokenizer treats both
// as separators, which is why a `gear` row can be written with either).
std::vector<std::string> split_line(const std::string& raw) {
    std::string line = raw.substr(0, raw.find(';'));
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Land type 0 fills all 8 slots; a later line overrides one. CONFIRMED in the original's
// script loader, which copies slot 0 into slots 1..7 right after writing it.
void skid_store(SkidPair (&table)[kLandTypeCount], int32_t land, int32_t low, int32_t high) {
    if (land == 0) {
        for (int i = 0; i < kLandTypeCount; ++i) { table[i].low = low; table[i].high = high; }
    } else if (land > 0 && land < kLandTypeCount) {
        table[land].low = low;
        table[land].high = high;
    }
}

} // namespace

void VehicleBox::build() {
    // Ring order, exactly as the original lays it out. The bottom face first (corners 0-3), so
    // `hnd wheels 15` selects it.
    const int32_t xs[8] = {xmin, xmin, xmax, xmax, xmin, xmin, xmax, xmax};
    const int32_t ys[8] = {ybottom, ybottom, ybottom, ybottom, ytop, ytop, ytop, ytop};
    const int32_t zs[8] = {zmin, zmax, zmax, zmin, zmin, zmax, zmax, zmin};
    for (int i = 0; i < 8; ++i) corner[i] = {xs[i], ys[i], zs[i]};
}

VehicleBox VehicleBox::from_ft(const Blob& ft) {
    VehicleBox b;
    if (ft.size() < 12) return b;
    auto s16 = [&](size_t off) {
        return static_cast<int32_t>(static_cast<int16_t>(
            static_cast<uint16_t>(ft[off]) | (static_cast<uint16_t>(ft[off + 1]) << 8)));
    };
    // (xmax, xmin, ybottom, ytop, zmax, zmin) - the engine reads exactly these six.
    // ybottom > ytop because the game's +Y points DOWN.
    b.xmax = s16(0);
    b.xmin = s16(2);
    b.ybottom = s16(4);
    b.ytop = s16(6);
    b.zmax = s16(8);
    b.zmin = s16(10);
    if (ft.size() >= 14) b.radius = s16(0x0c);
    // The 32-sector silhouette. A `.FT` is always exactly 118 bytes, so a short one is a shape
    // we could not load at all - leave the profile zeroed and the contact test will find nothing.
    if (ft.size() >= 0x76) {
        for (int i = 0; i < 32; ++i) {
            b.sector_reach[static_cast<size_t>(i)] = s16(static_cast<size_t>(kFtSectorReach + i * 2));
            b.sector_normal[static_cast<size_t>(i)] = ft[static_cast<size_t>(kFtSectorNormal + i)];
        }
    }
    b.build();
    return b;
}

void resolve_wheel_shapes(VehicleTuning& out,
                          const std::function<bool(const std::string&)>& has_variants) {
    for (VehicleSection& sec : out.sections) {
        sec.shape_table.clear();
        std::vector<bool> coloured;
        for (const std::string& n : sec.wheelsh) {
            if (has_variants(n)) {
                // The body's own convention, applied to a part: six repaints then the wreck.
                const std::string stem = n.substr(0, n.size() - 1);
                for (const char c : std::string("123456X")) {
                    sec.shape_table.push_back(stem + c);
                    coloured.push_back(true);
                }
            } else {
                sec.shape_table.push_back(n);
                coloured.push_back(false);
            }
        }
        for (WheelAttach& w : sec.wheels) {
            w.shape.clear();
            w.coloured = false;
            if (w.is_global_shape()) continue; // the player's character, not ours to pick
            const size_t sel = static_cast<size_t>(w.shape_sel & 0xF);
            if (sel >= sec.shape_table.size()) continue;
            w.shape = sec.shape_table[sel];
            w.coloured = coloured[sel];
        }
    }
}

bool parse_vehicle_script(const std::string& txt, const std::string& name, VehicleTuning& out) {
    out = VehicleTuning();
    out.name = name;

    std::vector<std::vector<std::string>> lines;
    {
        std::istringstream in(txt);
        std::string raw;
        while (std::getline(in, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::vector<std::string> t = split_line(raw);
            if (!t.empty()) lines.push_back(std::move(t));
        }
    }

    VehicleSection* section = nullptr;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::vector<std::string>& t = lines[li];
        const std::string kw = lower(t[0]);
        auto num = [&](size_t i) -> int32_t {
            int32_t v = 0;
            if (i < t.size()) parse_num(t[i], v);
            return v;
        };

        if (kw == "vehicle" && t.size() >= 3) {
            out.sections.push_back(VehicleSection());
            section = &out.sections.back();
            section->base = t[1];
            section->count = num(2);
        } else if (kw == "dash" && t.size() >= 2) {
            out.dash = t[1];
        } else if (kw == "ndial" && section && t.size() >= 2 &&
                   section->dials.size() < static_cast<size_t>(kMaxDashDials)) {
            // The original's list: "copy,point,pill,block,harea,varea," - an unknown type is dropped.
            static const char* kTypes[] = {"copy", "point", "pill", "block", "harea", "varea"};
            const std::string type = lower(t[1]);
            for (int k = 0; k < 6; ++k) {
                if (type != kTypes[k]) continue;
                DashDial d;
                d.type = k;
                for (int i = 1; i <= 10; ++i) d.r[i] = num(static_cast<size_t>(i) + 1);
                section->dials.push_back(d);
                break;
            }
        } else if (kw == "internal" && section) {
            for (int i = 0; i < 4 && i + 1 < static_cast<int>(t.size()); ++i)
                section->internal[i] = num(static_cast<size_t>(i) + 1);
        } else if (kw == "wheelsh" && section && t.size() >= 2) {
            section->wheelsh.push_back(t[1]);
        } else if (kw == "wheel" && section && t.size() >= 10) {
            WheelAttach w;
            w.flags = num(1);
            w.shape_sel = num(2);
            for (int i = 0; i < 3; ++i) w.pos[i] = num(static_cast<size_t>(3 + i));
            for (int i = 0; i < 3; ++i) w.rot[i] = num(static_cast<size_t>(6 + i));
            w.size = num(9);
            w.p9 = (t.size() > 10) ? num(10) : 0;
            section->wheels.push_back(w); // resolved below, once every `wheelsh` is in
        } else if (kw == "hnd" && t.size() >= 2) {
            const std::string sub = lower(t[1]);
            if (sub == "skid" && t.size() >= 6) {
                const std::string key = lower(t[2]);
                const int32_t land = num(3), low = num(4), high = num(5);
                if (key == "fvel") skid_store(out.fvel, land, low, high);
                else if (key == "fdam") skid_store(out.fdam, land, low, high);
                else if (key == "brk") skid_store(out.brk, land, low, high);
                else if (key == "svel") skid_store(out.svel, land, low, high);
                else if (key == "yavel") skid_store(out.yavel, land, low, high);
                else if (key == "iavel") skid_store(out.iavel, land, low, high);
            } else if (sub == "fx" && t.size() >= 10) {
                // `hnd fx <cat> <idx>, [pre|keep] [env|lev|vhc|front] <name> <6 numbers>`.
                // Parsed FROM THE END, because the two optional keywords in the middle make the
                // name's position vary: the last six tokens are always the numbers, and the one
                // before them is always the sample name. The scripts are hand-written and their
                // spacing is inconsistent, so counting from the front does not survive contact.
                const std::string cat = lower(t[2]);
                int fc = -1;
                if (cat == "engine") fc = 0;
                else if (cat == "nitro") fc = 1;
                else if (cat == "ground") fc = 2;
                else if (cat == "skid") fc = 3;
                else if (cat == "land") fc = 4;
                else if (cat == "hit") fc = 5;
                else if (cat == "check") fc = 6;
                const int32_t idx = num(3);
                if (fc < 0 || idx < 0 || idx >= kFxIndexCount) continue;
                const size_t n = t.size();
                FxSound fx;
                int32_t v[6] = {0, -1, 1, 0, 11025, 0};
                bool numeric = true;
                for (int i = 0; i < 6; ++i) numeric = numeric && parse_num(t[n - 6 + i], v[i]);
                if (!numeric) continue;
                fx.name = lower(t[n - 7]);
                for (size_t i = 4; i + 7 < n; ++i) {
                    const std::string w = lower(t[i]);
                    if (w == "pre") fx.preload = true;
                    else if (w == "keep") fx.keep = true;
                    else if (w == "env" || w == "lev" || w == "vhc" || w == "front") fx.dir = w;
                }
                fx.loop_start = v[0];
                fx.loop_end = v[1];
                fx.one_shot = v[2] != 0;
                fx.p4 = v[3];
                fx.freq = v[4] > 0 ? v[4] : 11025;
                fx.freq_full = v[5];
                out.fx[fc][idx] = std::move(fx);
            } else if (sub == "bounce" && t.size() >= 7) {
                const int32_t land = num(2);
                int32_t v[4];
                for (int i = 0; i < 4; ++i) v[i] = num(static_cast<size_t>(3 + i));
                if (land == 0) {
                    for (int s = 0; s < kLandTypeCount; ++s)
                        for (int i = 0; i < 4; ++i) out.bounce[s][i] = v[i];
                } else if (land > 0 && land < kLandTypeCount) {
                    for (int i = 0; i < 4; ++i) out.bounce[land][i] = v[i];
                }
            } else if (sub == "tilter" && t.size() >= 5) {
                TilterAxis a;
                a.mode = static_cast<int16_t>(num(3));
                a.a = num(4);
                a.b = (t.size() > 5) ? num(5) : 0;
                a.c = (t.size() > 6) ? num(6) : 0;
                if (lower(t[2]) == "z") out.tilter_z = a; else out.tilter_x = a;
            } else if (sub == "gear" && t.size() >= 4) {
                const bool forward = lower(t[2]) == "f";
                const int32_t count = num(3);
                // The tokenizer has no notion of a line: keep pulling numbers off the following
                // lines until 5*count of them have been read.
                std::vector<int32_t> flat;
                for (size_t i = 4; i < t.size(); ++i) {
                    int32_t v;
                    if (parse_num(t[i], v)) flat.push_back(v);
                }
                size_t next = li + 1;
                while (flat.size() < static_cast<size_t>(count) * 5 && next < lines.size()) {
                    int32_t v;
                    if (!parse_num(lines[next][0], v)) break;
                    for (const std::string& tok : lines[next])
                        if (parse_num(tok, v)) flat.push_back(v);
                    ++next;
                }
                li = next - 1;
                std::vector<GearRow>& rows = forward ? out.gear_f : out.gear_b;
                rows.clear();
                for (int r = 0; r < count && static_cast<size_t>(r) * 5 + 4 < flat.size(); ++r) {
                    const int32_t* p = &flat[static_cast<size_t>(r) * 5];
                    rows.push_back(GearRow{p[0], p[1], p[2], p[3], p[4]});
                }
            } else if (sub == "cogs") {
                for (int i = 0; i < 6; ++i) out.cogs[i] = num(static_cast<size_t>(2 + i));
            } else if (sub == "nitro") {
                for (int i = 0; i < 5; ++i) out.nitro[i] = num(static_cast<size_t>(2 + i));
            } else if (sub == "steering") {
                for (int i = 0; i < 6; ++i) out.steering[i] = num(static_cast<size_t>(2 + i));
            } else if (sub == "acceling") {
                for (int i = 0; i < 2; ++i) out.acceling[i] = num(static_cast<size_t>(2 + i));
            } else if (sub == "rpush") {
                for (int i = 0; i < 4; ++i) out.rpush[i] = num(static_cast<size_t>(2 + i));
            } else if (sub == "wheels") {
                for (int i = 0; i < 3; ++i) out.wheels[i] = static_cast<int16_t>(num(static_cast<size_t>(2 + i)));
            } else if (sub == "damage") {
                out.damage = num(2);
            }
        }
    }
    // Without the archive, the only thing left to go on is the name: a shape that owns the team
    // variants is written `<stem>1`. On this game's data that guess is exact - the only two such
    // names are BUGLWHL1/BUGRWHL1 and RUDDER1 - but `load_vehicle` checks the files anyway.
    resolve_wheel_shapes(out, [](const std::string& n) {
        return !n.empty() && n.back() == '1';
    });
    return !out.sections.empty();
}

bool load_vehicle(const std::string& game_dir, const std::string& name, VehicleTuning& out,
                  ArchivePtr* archive_out) {
    ArchivePtr vhc;
    try {
        vhc = game_archive(game_dir, name + ".VHC");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vehicle %s: %s\n", name.c_str(), e.what());
        return false;
    }
    auto find = [&](const std::string& fn) { return find_member(*vhc, fn); };
    const Blob* script = find(name + ".TXT");
    if (!script) return false;
    VehicleTuning parsed;
    if (!parse_vehicle_script(std::string(script->begin(), script->end()), name, parsed))
        return false;

    // Now that the archive is open, answer the variant question from the files rather than from
    // the name: a shape owns the team variants when its `<stem>X` wreck is there beside it.
    resolve_wheel_shapes(parsed, [&](const std::string& n) {
        return !n.empty() && n.back() == '1' && find(n.substr(0, n.size() - 1) + "X.3DW");
    });

    // The contact box is the body shape's .FT, the FIRST colour variant - every variant shares
    // the geometry, so any of them would do.
    if (!parsed.sections.empty()) {
        const Blob* ft = find(parsed.sections[0].base + "1.FT");
        if (ft) parsed.box = VehicleBox::from_ft(*ft);
    }

    // The wheel radius comes from the wheel shape's own .FT. The mesh is authored with its axle
    // along Z (the `wheel` lines rotate it 90 degrees about Y to stand it up), so X and Y are the
    // round directions.
    for (const VehicleSection& sec : parsed.sections) {
        for (const WheelAttach& w : sec.wheels) {
            if (!w.is_wheel() || w.shape.empty()) continue;
            const Blob* ft = find(w.shape + ".FT");
            if (!ft) continue;
            const VehicleBox wb = VehicleBox::from_ft(*ft);
            parsed.wheel_radius = (wb.ybottom - wb.ytop) / 2;
            break;
        }
        if (parsed.wheel_radius > 0) break;
    }

    out = std::move(parsed);
    if (archive_out) *archive_out = std::move(vhc);
    return true;
}

} // namespace bigred
