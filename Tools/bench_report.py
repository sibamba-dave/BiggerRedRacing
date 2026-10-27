"""Bench reporting for Tools/bench.ps1: averages the run's `perf:` log lines and the CSV
profiler's thread times, prints a one-line summary, and stamps the label + summary into the
screenshot so every saved image says what it is.

Usage: python Tools/bench_report.py --log run.log [--csv run.csv] [--image shot.png] --label "text"
"""
import argparse
import csv
import re
import sys
from pathlib import Path


def parse_log(path: Path):
    pat = re.compile(r"perf: ([\d.]+) fps, race at ([\d.]+)x real time, physics at ([\d.]+)x")
    rows = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = pat.search(line)
            if m:
                rows.append(tuple(float(g) for g in m.groups()))
    if len(rows) > 1:
        rows = rows[1:]  # the first window is shader-compile warm-up
    if not rows:
        return None
    n = len(rows)
    return {
        "fps": sum(r[0] for r in rows) / n,
        "fps_min": min(r[0] for r in rows),
        "race": sum(r[1] for r in rows) / n,
        "physics": sum(r[2] for r in rows) / n,
        "windows": n,
    }


def parse_csv(path: Path):
    """Average the standard thread-time columns of a UE csv profiler capture (ms)."""
    want = {"frametime": "frame", "gamethreadtime": "game", "renderthreadtime": "render",
            "gputime": "gpu", "rhithreadtime": "rhi"}
    csv.field_size_limit(1 << 30)  # UE emits very long event fields
    sums, counts, index = {}, {}, {}
    with open(path, newline="", encoding="utf-8", errors="ignore") as f:
        reader = csv.reader(f)
        try:
            headers = next(reader)
        except StopIteration:
            return None
        for i, h in enumerate(headers):
            key = h.split("/")[-1].strip().lower()
            if key in want:
                index[i] = want[key]
        if not index:
            return None
        rows = [r for r in reader if r and not r[0].startswith("//")]
        rows = rows[len(rows) // 4:]  # the first quarter is level load and shader warm-up
        for row in rows:
            for i, name in index.items():
                if i < len(row):
                    try:
                        v = float(row[i])
                    except ValueError:
                        continue
                    sums[name] = sums.get(name, 0.0) + v
                    counts[name] = counts.get(name, 0) + 1
    return {name: sums[name] / counts[name] for name in sums if counts.get(name)}


def stamp(image: Path, lines):
    from PIL import Image, ImageDraw, ImageFont
    im = Image.open(image).convert("RGB")
    size = max(14, im.height // 50)
    try:
        font = ImageFont.truetype("arial.ttf", size)
    except OSError:
        font = ImageFont.load_default()
    draw = ImageDraw.Draw(im, "RGBA")
    pad = size // 2
    wrapped = []
    for t in lines:  # split overlong lines at their " | " separators
        while draw.textbbox((0, 0), t, font=font)[2] > im.width - 2 * pad and " | " in t:
            cut = t.rfind(" | ", 0, len(t) * (im.width - 2 * pad) // draw.textbbox((0, 0), t, font=font)[2])
            if cut <= 0:
                break
            wrapped.append(t[:cut])
            t = t[cut + 3:]
        wrapped.append(t)
    lines = wrapped
    widths = [draw.textbbox((0, 0), t, font=font)[2] for t in lines]
    band_h = (size + pad // 2) * len(lines) + pad
    top = im.height - band_h
    draw.rectangle([0, top, max(widths) + 2 * pad, im.height], fill=(0, 0, 0, 170))
    for i, t in enumerate(lines):
        draw.text((pad, top + pad // 2 + i * (size + pad // 2)), t, fill=(255, 255, 255), font=font)
    im.save(image)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True, type=Path)
    ap.add_argument("--csv", type=Path)
    ap.add_argument("--image", type=Path)
    ap.add_argument("--label", required=True)
    args = ap.parse_args()

    perf = parse_log(args.log)
    if perf is None:
        print(f"{args.label}: no perf lines in {args.log}", file=sys.stderr)
        sys.exit(1)
    summary = (f"{perf['fps']:.1f} fps avg ({perf['fps_min']:.1f} min of {perf['windows']} windows), "
               f"race {perf['race']:.2f}x, physics {perf['physics']:.2f}x")
    threads = parse_csv(args.csv) if args.csv and args.csv.exists() else None
    if threads:
        parts = [f"{k} {threads[k]:.2f} ms" for k in ("frame", "game", "render", "rhi", "gpu") if k in threads]
        summary += " | " + ", ".join(parts)
    print(f"{args.label}: {summary}")
    if args.image and args.image.exists():
        stamp(args.image, [args.label, summary])


if __name__ == "__main__":
    main()
