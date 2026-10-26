"""Reads the Big Red Racing CD from a raw image (2352-byte sectors, no cue sheet).

Usage: python Tools/brr_cd.py <BRR_CD.img> <out_dir> [--rip <rip_dir>]

- Lists the data track's ISO 9660 files and copies any the rip folder lacks into <rip_dir>
  (never overwriting), so nothing on the CD is missed.
- Splits the audio after the data track into tracks at runs of digital silence (the 2-second gaps
  a pressed CD has between tracks) and writes <out_dir>/Track<NN>.wav (44.1 kHz 16-bit stereo).
- Writes <out_dir>/BRR_CD.cue describing the image (Mode 2 data track plus the audio tracks), which
  DOSBox-X mounts with CD music.

The game's music and video are the original's content: keep the output out of git.
No third-party dependencies.
"""
import argparse
import os
import struct
import sys

SECTOR = 2352
SILENT_GAP = 75  # sectors (1 s) of exact zeros separate tracks; music never holds that long


def data_reader(f):
    """Returns read(lba) -> 2048 bytes of user data, for the track's sector mode."""
    f.seek(0)
    head = f.read(16)
    offset = 24 if head[15] == 2 else 16  # Mode 2 Form 1: 16-byte header + 8-byte subheader

    def read(lba, count=1):
        out = bytearray()
        for i in range(count):
            f.seek((lba + i) * SECTOR + offset)
            out += f.read(2048)
        return bytes(out)
    return read, head[15]


def iso_files(read):
    """Yields (path, lba, size) for every file of the ISO 9660 volume."""
    pvd = read(16)
    assert pvd[1:6] == b"CD001", "no ISO 9660 volume"
    root = pvd[156:190]

    def walk(lba, size, path):
        data = read(lba, (size + 2047) // 2048)
        pos = 0
        while pos < size:
            n = data[pos]
            if n == 0:  # records never cross a sector: skip to the next one
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            ext, length, flags, nlen = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0], rec[25], rec[32]
            name = rec[33:33 + nlen]
            pos += n
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            if flags & 2:
                yield from walk(ext, length, f"{path}{name}/")
            else:
                yield f"{path}{name}", ext, length

    ext, length = struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0]
    yield from walk(ext, length, "")


def audio_tracks(f, first, total):
    """[(start_lba, end_lba)] of the audio tracks from `first`, split at silent gaps."""
    tracks, start, run = [], None, 0
    for lba in range(first, total):
        f.seek(lba * SECTOR)
        silent = not any(f.read(SECTOR))
        if silent:
            run += 1
            if start is not None and run == SILENT_GAP:
                tracks.append((start, lba - SILENT_GAP + 1))
                start = None
        else:
            if start is None:
                start = lba
            run = 0
    if start is not None:
        tracks.append((start, total))
    # A gap can hold a stray non-silent sector or two; those are not tracks.
    return [(s, e) for s, e in tracks if e - s >= SILENT_GAP]


def write_wav(path, f, start, end):
    frames = (end - start) * SECTOR
    with open(path, "wb") as out:
        out.write(b"RIFF" + struct.pack("<I", 36 + frames) + b"WAVEfmt " +
                  struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16) + b"data" + struct.pack("<I", frames))
        for lba in range(start, end):
            f.seek(lba * SECTOR)
            out.write(f.read(SECTOR))


def msf(lba):
    return f"{lba // 4500:02d}:{lba // 75 % 60:02d}:{lba % 75:02d}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--rip")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    f = open(a.image, "rb")
    f.seek(0, 2)
    total = f.tell() // SECTOR
    read, mode = data_reader(f)
    blocks = struct.unpack("<I", read(16)[80:84])[0]
    print(f"{a.image}: {total} sectors, data track Mode {mode}, {blocks} blocks")

    files = list(iso_files(read))
    print(f"{len(files)} files on the CD")
    if a.rip:
        copied = 0
        for path, lba, size in files:
            dest = os.path.join(a.rip, *path.split("/"))
            if os.path.exists(dest):
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as out:
                out.write(read(lba, (size + 2047) // 2048)[:size])
            copied += 1
            print(f"  copied {path} ({size} bytes)")
        print(f"{copied} file(s) were missing from {a.rip}")

    tracks = audio_tracks(f, blocks, total)
    cue = [f'FILE "{os.path.basename(a.image)}" BINARY', f"  TRACK 01 MODE{mode}/2352", "    INDEX 01 00:00:00"]
    for n, (start, end) in enumerate(tracks, 2):
        name = os.path.join(a.out, f"Track{n:02d}.wav")
        write_wav(name, f, start, end)
        print(f"  Track {n:02d}: {msf(start)} - {(end - start) / 75:6.1f} s -> {name}")
        cue += [f"  TRACK {n:02d} AUDIO", f"    INDEX 01 {msf(start)}"]
    with open(os.path.join(a.out, "BRR_CD.cue"), "w") as out:
        out.write("\n".join(cue) + "\n")
    print(f"cue sheet: {os.path.join(a.out, 'BRR_CD.cue')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
