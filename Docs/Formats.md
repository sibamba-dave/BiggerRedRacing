# Big Red Racing data formats

Status key: ✅ implemented in `BRRFormats` · 📄 documented (ORR) · ❓ unknown

## Archive container (.LEV .ENV .VHC .SCN .WAP .TEA) ✅
No header. Entries: `char name[] '\0'`, `u32 packed`, `u32 unpacked`, `u8 method`, `data[packed]`; an empty name ends the file.
- 0 stored · 1 RLE (`c<0x80`: copy c+1 literals; else repeat next byte c−125 times)
- 2–6 LZSS: flag byte LSB-first (1 = literal), refs are 2 bytes; offset bits = 6−method, ring = 2^(offset_bits+8), lengths 2..(2^(8−offset_bits)+2), zero-filled ring.
- Verified: 78 archives / 2777 members in the shipped data decode exactly (`BRR.Formats.Archive.DecodeAll`).

## Members
| Member | Format | Status |
|---|---|---|
| `.RAW` | 768-byte 6-bit palette + 320×200 8-bit | 📄 |
| `.TM` | 256×192 8-bit texture page, index 0 transparent | 📄 |
| `.COL` | 768-byte 6-bit VGA palette (×4 for 8-bit) | 📄 |
| `.SPR` | `u16 count; u32 offsets[]`; each sprite `u16 w,h,x,y` + w×h pixels | 📄 |
| `MAP.MPH` | 256×256 u8 heights; 16 units/step, 512 units/cell | 📄 |
| `MAP.MPC` | 256×256 ground codes; ≥240 = hole | 📄 |
| `MAP.CHT` | 3×256 tables; [512..] = tile index (≥192 flat colour), 32px tiles in 8×6 atlases | 📄 |
| `MAP.SHL` | 13-byte zero-padded shape names | 📄 |
| `MAP.DAT` | 22-byte placements: i8 shape, i32 x,y,z (+Y down), i16 BAM rot x,y,z, u8 variant, u8 category, u8 param | 📄 |
| `MAP.CSE` | 16-byte gates: i32 x,z, i16 angle(1024/turn), length, half_depth, half_depth_delta | 📄 |
| `MAP.GRP` | grouping | ❓ |
| `.3DW` | int32 header + threaded display list (see ORR `threedw.h`); vertices ×2 scale, Y inverted | 📄 (tag 3, tag-13 trailer ❓) |
| `.FT` | 118 bytes: bounds, radius, surface profile or blocker | 📄 partial |
| `n.REC` | 40-byte header + int16 deltas per 28 ms tick | 📄 |
| `.TXT` | level / vehicle / screen scripts | 📄 |
| `VICTORY.MVI` | `MVI\0`, u16 320, 200, 178 frames, palette @0x14, RLE frames | ❓ |
| WAV | RIFF PCM 8-bit mono 11025 Hz | trivial |

Timing: game tick 14 ms, physics 28 ms.
