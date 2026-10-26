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
| `VICTORY.MVI` | end video: palette + RLE delta frames + WAV effects, see [Videos](#videos-introsndrpl-victorymvi) | 📄 `Tools/brr_video.py` |
| WAV | RIFF PCM 8-bit mono 11025 Hz | trivial |

Timing: game tick 14 ms, physics 28 ms.

## Videos (INTROSND.RPL, VICTORY.MVI)
Both decode with `python Tools/brr_video.py <file> <out_dir>` (PNG frames + `audio.wav`); verified by eye on first/middle/last frames.

**`VIDEO/INTROSND.RPL`** (intro, played by `VIDEO/VIDTHUMP.EXE`, Eidos `eidosply.c`/`thump122.asm`): Acorn Replay ARMovie.
- Header: newline-separated text fields, value first: `ARMovie`, name, copyright, author, video format `122`, 320, 200, 8 bpp, 25 fps, sound format `101` (text says "none", 0 bits), 22050 Hz, 1 channel, 1 frame per chunk, 3299 (last chunk index: 3300 chunks), even/odd chunk size, catalogue offset (24720660), sprite offset/size, key frames 0.
- Catalogue: one `offset,video_size;audio_size` line per chunk. Chunk = Escape 122 frame then 735 bytes of audio, 4-aligned.
- Escape 122 frame: `u32 0x116` (0x100 + 22), `u32 size`, `u16 pal_bytes`, palette (6-bit RGB from entry 0; 0 = keep), then an LSB-first bitstream. 8×8 blocks in raster order; a skip count `ecode` (1 bit 0 → 0; else 3 bits +1, 7 → 7 bits +8, 127 → 12 bits +135) is read before a block when none is pending; N > 0 skips this and the next N−1 blocks and the block after the run is coded without a count. A coded block is a 4×4 grid of 2×2 cells (mask bit i = cell i, row-major, LSB first): while a 0 bit, one cell pattern + a 16-bit mask of cells to paint with it; then a bit, if 0 a 16-bit mask with one pattern per set cell. Cell pattern: 4-bit mask; 0 / 15 → solid colour `2·u7` / `2·u7+1`; else two u8 colours, mask bit (TL, TR, BL, BR) picks one. Algorithm after NihAV's `escape.rs`, which follows the original player.
- Audio: Eidos ADPCM despite the header: IMA step/index tables, high nibble first, `diff = (code & 7) · step >> 2`, sign bit 8, state carried across chunks. 1470 samples per chunk, 4 851 000 in all: 220 s at 22050 Hz, so the movie runs at 15 chunks/s, not the header's 25 fps (132 s). The spectrum falls off at ~5.5 kHz (an 11 kHz source resampled), which fits 22050 Hz being right. Presumably the player paces video by the sound (it keeps "sound/video queues"); not confirmed by running it.

**`RACING/VICTORY.MVI`** (end video, `RACING.EXE`): own format, 178 frames, 10.5 s at 17 fps (fps field inferred).
- Header 16 bytes: `"MVI\0"`, `u16 width 320, height 200, frames 178, 17`, `u32 0`.
- Per frame: records until a 0 byte, then `u16 size` + picture. Records: `1` = 768-byte 6-bit palette (only before frame 0); `2` = sound effect: `u16 size`, `u8 param` (27/77/127/227, probably pan), RIFF WAV (PCM 8-bit mono 11025 Hz) of `size` bytes, started with this frame (9 effects).
- Picture: RLE over 64000 pixels, row-major: `c ≥ 0x80` → c−0x80 literals; `1..0x7F` → run of c × next byte; `0` → `u16 count`, byte. Index 0 leaves the pixel as it was (frame 0 has none; later frames are deltas mostly of 0).
