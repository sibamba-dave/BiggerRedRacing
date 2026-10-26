# Big Red Racing data formats

Status key: ✅ exposed by `BRRFormats` (UE types, tested) · 📄 parsed by vendored ORR code only · ❓ unknown

## Coordinates and scale
Game frame: world units, +Y down, right-handed. UE: `X = z, Y = x, Z = -y`, × `BRR::CmPerUnit` (2.5).
The JEEP is 155 units long (3.9 m); a terrain cell is 512 units (12.8 m); a 256-cell map ~3.3 km.
Game yaw (x = sin, z = cos, BAM16) equals UE yaw. Handedness flips, so triangle winding is reversed.

## Archive container (.LEV .ENV .VHC .SCN .WAP .TEA) ✅
No header. Entries: `char name[] '\0'`, `u32 packed`, `u32 unpacked`, `u8 method`, `data[packed]`; an empty name ends the file.
- 0 stored · 1 RLE (`c<0x80`: copy c+1 literals; else repeat next byte c−125 times)
- 2–6 LZSS: flag byte LSB-first (1 = literal), refs are 2 bytes; offset bits = 6−method, ring = 2^(offset_bits+8), lengths 2..(2^(8−offset_bits)+2), zero-filled ring.
- Verified: 78 archives / 2777 members in the shipped data decode exactly (`BRR.Formats.Archive.DecodeAll`).

## Members
| Member | Format | Status |
|---|---|---|
| `.RAW` | 768-byte 6-bit palette + 320×200 8-bit | ✅ |
| `.TM` | 256×192 8-bit texture page, index 0 transparent | ✅ |
| `.COL` | 768-byte 6-bit VGA palette (×4 for 8-bit) | ✅ |
| `.SPR` | `u16 count; u32 offsets[]`; each sprite `u16 w,h,x,y` + w×h pixels (`.FNT`: 96 glyphs) | ✅ |
| `MAP.MPH` | 256×256 u8 heights; 16 units/step, 512 units/cell | ✅ (terrain mesh) |
| `MAP.MPC` | 256×256 ground codes; ≥240 = hole | ✅ (terrain mesh) |
| `MAP.CHT` | 3×256 tables; [512..] = tile index (≥192 flat colour), 32px tiles in 8×6 atlases | ✅ (terrain mesh) |
| `MAP.SHL` | 13-byte zero-padded shape names | ✅ |
| `MAP.DAT` | 22-byte placements: i8 shape, i32 x,y,z (+Y down), i16 BAM rot x,y,z, u8 variant, u8 category, u8 param | ✅ placements (per-instance rotation still to verify visually) |
| `MAP.CSE` | 16-byte gates: i32 x,z, i16 angle(1024/turn), length, half_depth, half_depth_delta | ✅ |
| `MAP.GRP` | grouping | ❓ |
| `.3DW` | int32 header + threaded display list (see ORR `threedw.h`); vertices ×2 scale, Y inverted | ✅ via ORR scene builders (decor baked; vehicle body + wheels); tag 3, tag-13 trailer ❓ |
| `.FT` | 118 bytes: bounds, radius, surface profile or blocker | 📄 partial |
| `n.REC` | 40-byte header + int16 deltas per 28 ms tick | ✅ raw records |
| `.TXT` | level / vehicle / screen scripts | ✅ level grid/laps/cars/unsafe/ghost rates, TOURN.TXT; vehicle handling 📄 |
| `VICTORY.MVI` | `MVI\0`, u16 320, 200, 178 frames, palette @0x14, RLE frames | ❓ |
| WAV | RIFF PCM 8-bit mono 11025 Hz | trivial |

Timing: game tick 14 ms, physics 28 ms.
