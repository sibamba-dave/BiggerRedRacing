"""Decodes Big Red Racing's two videos to PNG frames and a WAV.

Usage: python Tools/brr_video.py <INTROSND.RPL|VICTORY.MVI> <out_dir> [--step N]

Writes out_dir/frame_0000.png ... (the 8-bit palette expanded to RGB) and out_dir/audio.wav when
the file has sound, then prints a summary. --step N writes only every Nth frame (all are still
decoded, as both formats are delta-coded). No third-party dependencies. See Docs/Formats.md.

INTROSND.RPL is an Acorn Replay "ARMovie": a text header of one field per line (video format 122
= Eidos Escape 122, 320x200, 8 bpp, fps, sound format, chunk count, offset of the chunk catalogue),
then chunks, then the catalogue of "offset,video_size;audio_size" lines. Each chunk is one video
frame followed by its audio. Escape 122 frame: u32 0x116, u32 size, u16 palette bytes, that many
bytes of 6-bit RGB (from entry 0; 0 keeps the old palette), then an LSB-first bitstream of 8x8
blocks in raster order, each a skip run or a 4x4 grid of 2x2 cells painted in passes. The header
says "sound format 101 - none, 0 bits", but each chunk carries 735 bytes of Eidos 4-bit ADPCM
(IMA tables, high nibble first, diff = (code & 7) * step >> 2), i.e. 1470 samples per chunk:
at the header's 22050 Hz that is 15 chunks a second, not the header's 25 fps (see Formats.md).

VICTORY.MVI: 16-byte header ("MVI\\0", u16 width, height, frames, 17 = fps?, u32 0), then per
frame a list of records ended by a 0 byte (1: 768-byte 6-bit palette; 2: u16 size, u8 param
(pan?), RIFF WAV of that size, started with this frame), then u16 size and an RLE delta picture
of that size in which index 0 leaves the pixel unchanged.
"""
import argparse
import os
import struct
import zlib

IMA_STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8] * 2


def write_png(path, width, height, rows):
    """8-bit RGB PNG from rows of bytes."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def write_wav(path, rate, channels, bits, data):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I4s", 36 + len(data), b"WAVE"))
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * bits // 8,
                                      channels * bits // 8, bits))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def vga_palette(data):
    """6-bit VGA triples to a list of 3-byte RGB strings."""
    return [bytes((v << 2 | v >> 4) & 255 for v in data[i:i + 3]) for i in range(0, len(data) - 2, 3)]


def save_frame(out_dir, n, width, height, pixels, palette):
    rgb = b"".join(palette[i] for i in pixels)
    rows = [rgb[y * width * 3:(y + 1) * width * 3] for y in range(height)]
    write_png(os.path.join(out_dir, f"frame_{n:04d}.png"), width, height, rows)


# --- ARMovie / Escape 122 ---------------------------------------------------------------------

class Bits:
    """LSB-first bit reader."""

    def __init__(self, data, pos):
        self.data, self.pos = data + b"\0\0\0\0", pos * 8

    def get(self, n):
        p = self.pos
        v = int.from_bytes(self.data[p >> 3:(p >> 3) + 4], "little") >> (p & 7)
        self.pos = p + n
        return v & ((1 << n) - 1)

    def ecode(self):
        if not self.get(1):
            return 0
        v = self.get(3)
        if v != 7:
            return v + 1
        v = self.get(7)
        if v != 127:
            return v + 8
        return self.get(12) + 135


def escape122_cell(bits):
    """One 2x2 cell: (top-left, top-right, bottom-left, bottom-right)."""
    mask = bits.get(4)
    if mask in (0, 15):
        c = bits.get(7) * 2 + (mask == 15)
        return c, c, c, c
    c = (bits.get(8), bits.get(8))
    return tuple(c[mask >> i & 1] for i in range(4))


def escape122_frame(chunk, frame, palette, width, height):
    """Decodes one frame into `frame` (palette indices) and `palette`, in place."""
    tag, size, pal_bytes = struct.unpack_from("<IIH", chunk)
    assert tag == 0x116, f"not Escape 122 ({tag:#x})"
    if pal_bytes:
        new = vga_palette(chunk[10:10 + min(pal_bytes, 768)])
        palette[:len(new)] = new
    bits, skip = Bits(chunk[:size], 10 + pal_bytes), None
    for by in range(0, height, 8):
        for bx in range(0, width, 8):
            if skip is None:
                skip = bits.ecode()
            if skip:
                skip -= 1  # at 0 the next block is coded with no count of its own
                continue
            skip = None
            cells = [None] * 16
            while not bits.get(1):
                cell, mask = escape122_cell(bits), bits.get(16)
                for i in range(16):
                    if mask >> i & 1:
                        cells[i] = cell
            if not bits.get(1):
                mask = bits.get(16)
                for i in range(16):
                    if mask >> i & 1:
                        cells[i] = escape122_cell(bits)
            for i, cell in enumerate(cells):
                if cell is None:
                    continue
                o = (by + (i >> 2) * 2) * width + bx + (i & 3) * 2
                frame[o], frame[o + 1], frame[o + width], frame[o + width + 1] = cell


def escape_adpcm(data, state):
    """Eidos 4-bit ADPCM (mono) to signed 16-bit samples; state = [predictor, step index]."""
    pred, step, out = state[0], state[1], []
    for b in data:
        for code in (b >> 4, b & 15):
            diff = (code & 7) * IMA_STEPS[step] >> 2
            pred = max(-32768, min(32767, pred - diff if code & 8 else pred + diff))
            step = max(0, min(88, step + IMA_INDEX[code]))
            out.append(pred)
    state[0], state[1] = pred, step
    return struct.pack(f"<{len(out)}h", *out)


def decode_rpl(data, out_dir, step):
    lines = data[:1024].split(b"\n")
    field = lambda i: lines[i].split()[0].decode("latin1")
    width, height, fps = int(field(5)), int(field(6)), float(field(8))
    rate, channels = int(field(10)), int(field(11))
    if field(4) != "122":
        raise SystemExit(f"video format {field(4)} is not Escape 122")
    cat_offset = int(field(17))
    catalogue = [tuple(map(int, e.replace(b";", b",").split(b",")))
                 for e in data[cat_offset:].split() if b"," in e]
    frame, palette = bytearray(width * height), [b"\0\0\0"] * 256
    pcm, state, frames = bytearray(), [0, 0], 0
    for offset, vsize, asize in catalogue:  # chunks 0..N, one more than the header's count
        escape122_frame(data[offset:offset + vsize], frame, palette, width, height)
        if frames % step == 0:
            save_frame(out_dir, frames, width, height, frame, palette)
        frames += 1
        pcm += escape_adpcm(data[offset + vsize:offset + vsize + asize], state)
    samples = len(pcm) // 2
    print(f"ARMovie Escape 122 {width}x{height}, {frames} frames, header {fps:g} fps ({frames / fps:.1f} s)")
    if samples:
        write_wav(os.path.join(out_dir, "audio.wav"), rate, channels, 16, bytes(pcm))
        print(f"sound: Escape ADPCM, {samples} samples at {rate} Hz mono ({samples / rate:.1f} s), "
              f"{samples // frames} per chunk = {rate * frames / samples:g} fps")


# --- MVI --------------------------------------------------------------------------------------

def mvi_frame(data, o, end, frame):
    """RLE delta: c<0x80 run of c, 0 = u16 run, c>=0x80 c-0x80 literals; index 0 leaves the pixel."""
    p = 0
    while o < end:
        c = data[o]
        if c >= 0x80:
            n = c - 0x80
            for i, v in enumerate(data[o + 1:o + 1 + n]):
                if v:
                    frame[p + i] = v
            o += 1 + n
        else:
            if c == 0:
                n, v = struct.unpack_from("<HB", data, o + 1)
                o += 4
            else:
                n, v = c, data[o + 1]
                o += 2
            if v:
                frame[p:p + n] = bytes((v,)) * n
        p += n
    return p


def decode_mvi(data, out_dir, step):
    magic, width, height, count, fps = struct.unpack_from("<4sHHHH", data)
    assert magic == b"MVI\0"
    frame, palette, o, sounds = bytearray(width * height), [b"\0\0\0"] * 256, 0x10, []
    for n in range(count):
        while data[o]:  # records shown with this frame, ended by tag 0
            tag = data[o]
            if tag == 1:
                palette = vga_palette(data[o + 1:o + 769])
                o += 769
            elif tag == 2:
                size, param = struct.unpack_from("<HB", data, o + 1)
                sounds.append((n, param, data[o + 4:o + 4 + size]))
                o += 4 + size
            else:
                raise SystemExit(f"frame {n}: unknown record tag {tag} at {o:#x}")
        size = struct.unpack_from("<H", data, o + 1)[0]
        assert mvi_frame(data, o + 3, o + 3 + size, frame) == width * height, f"frame {n} size"
        o += 3 + size
        if n % step == 0:
            save_frame(out_dir, n, width, height, frame, palette)
    print(f"MVI {width}x{height}, {count} frames at {fps} fps? ({count / fps:.1f} s), {len(data) - o} bytes left")
    if not sounds:
        return
    rate, mix = 11025, []
    for n, param, wav in sounds:
        rate = struct.unpack_from("<I", wav, 24)[0]
        at = wav.find(b"data")
        pcm = wav[at + 8:at + 8 + struct.unpack_from("<I", wav, at + 4)[0]]
        start = n * rate // fps
        mix.extend([0] * (start + len(pcm) - len(mix)))
        for i, v in enumerate(pcm):
            mix[start + i] += v - 128
        print(f"  sound at frame {n}: {len(pcm)} samples ({len(pcm) / rate:.2f} s), param {param}")
    write_wav(os.path.join(out_dir, "audio.wav"), rate, 1, 8,
              bytes(max(0, min(255, v + 128)) for v in mix))
    print(f"sound: {len(sounds)} 8-bit {rate} Hz effects mixed to {len(mix) / rate:.1f} s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("out_dir")
    ap.add_argument("--step", type=int, default=1)
    a = ap.parse_args()
    data = open(a.video, "rb").read()
    os.makedirs(a.out_dir, exist_ok=True)
    if data.startswith(b"ARMovie"):
        decode_rpl(data, a.out_dir, a.step)
    elif data.startswith(b"MVI\0"):
        decode_mvi(data, a.out_dir, a.step)
    else:
        raise SystemExit("neither ARMovie nor MVI")


if __name__ == "__main__":
    main()
