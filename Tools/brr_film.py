"""The remastered intro: the original's cut, its race shots filmed again in the engine.

Usage:
  python Tools/brr_film.py shots <frames_dir> <shots.json>
      Finds the cuts in the decoded intro (Tools/brr_video.py, every frame) and what each shot is:
      'title' (the flat motion graphics: few colours), 'live' (the filmed inserts: a picture in a
      black frame) or 'race' (gameplay), and for a race shot whether it is on 'water' or 'land'.
      Writes the shot list: start and end frame (15 a second, the intro's own rate), kind, ground.
  python Tools/brr_film.py plan <shots.json> <plan.json>
      Gives every race shot a circuit of its ground and a camera, and groups them by circuit for
      the engine to film (-BRRFilm, ABRRFilm): each circuit's shots one after another in one race.
  python Tools/brr_film.py assemble <plan.json> <frames_dir> <film_dir> <audio.wav> <out.mp4>
      Cuts it together at the original's timings over its sound: the filmed race shots (30 a
      second), the title cards and the inserts from their own frames, upscaled (title cards
      smoothed as flat art by the engine's BRR::SmoothPixelArt, see -run=BRRUpscaleFrames).
  Tools/upscale/.venv/Scripts/python -I Tools/brr_film.py upscale <frames_dir> <model.pth> <out_dir>
      Every frame through a 4x model (the victory video's comic panels: 4x-UltraSharp keeps their
      lines whole), to 1920x1200.
  python Tools/brr_film.py encode <frames_dir> <fps> <audio.wav> <out.mp4>
      The frames as a 1080p video over the sound (the remastered victory video).
  Tools/upscale/.venv/Scripts/python -I Tools/brr_film.py homevideo <plan.json> <frames_dir> <model.pth> <out_dir>
      The filmed inserts upscaled, but left looking like home video: the model's 4x (the project's
      Remacri) of the deblocked frame only half way over a plain smooth upscale, the camera's grain put back, its colour
      and its blown highlights kept. Writes <out_dir>/frame_NNNN.png at 1920x1200 for assemble.

The original's frames, sound and the result stay out of git (README): only the shot list and the
plan, which are timings, are kept (Tools/film/).
"""
import argparse
import glob
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

FPS = 15


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def classify(frame):
    """('title' | 'live' | 'race', ground) for one 320x200 frame."""
    h, w, _ = frame.shape
    flat = frame.reshape(-1, 3).astype(np.int64)
    colours = len(np.unique(flat[:, 0] * 65536 + flat[:, 1] * 256 + flat[:, 2]))
    edges = np.concatenate([frame[:, :24].reshape(-1, 3), frame[:, -24:].reshape(-1, 3)])
    dark_edges = (edges.max(axis=1) < 24).mean()
    middle = frame[:, 60:260].reshape(-1, 3)
    if dark_edges > 0.9 and middle.max(axis=1).mean() > 40:
        return 'live', None
    # A title card: a few flat colours, the commonest of them pale (the pinks and white) or black;
    # flat water is as few colours, but blue or green.
    keys, counts = np.unique(flat[:, 0] * 65536 + flat[:, 1] * 256 + flat[:, 2], return_counts=True)
    order = np.argsort(counts)[::-1]
    top = keys[order[0]]
    tr, tg, tb = top >> 16, (top >> 8) & 255, top & 255
    pale = tr >= 190 and tg >= 140 and tb >= 140 and tb <= tr + 5  # pink or white, not a lilac sky
    black = max(tr, tg, tb) < 40
    if colours < 24 or (counts[order[:3]].sum() / len(flat) > 0.7 and (pale or black)):
        return 'title', None
    low = frame[h // 3:].reshape(-1, 3).astype(np.float32)
    r, g, b = low[:, 0], low[:, 1], low[:, 2]
    blue = (b > r * 1.5) & (b > g * 0.9) & (b > 60)
    teal = (g > r * 1.5) & (b > r * 1.2) & (g < 170) & (g > 50)
    water = (blue | teal).mean()
    return 'race', ('water' if water > 0.3 else 'land')


OVERRIDES = {1259: 'live', 1271: 'live', 1279: 'live', 1319: 'title'}


def shots(frames_dir, out):
    files = sorted(glob.glob(os.path.join(frames_dir, 'frame_*.png')))
    prev = None
    diffs = []
    kinds = []
    for f in files:
        cur = load(f)
        diffs.append(0.0 if prev is None else float(np.abs(cur - prev).mean()))
        kinds.append(classify(cur)[0])
        prev = cur
    d = np.array(diffs)
    # A cut: a jump well above the motion around it.
    cuts = [0]
    for i in range(1, len(d)):
        local = np.median(d[max(1, i - 8):i + 8])
        if d[i] > max(18.0, local * 3.0) and i - cuts[-1] >= 4:
            cuts.append(i)
    # ...and where the kind of picture changes and stays changed (a title card cut into a race).
    for i in range(1, len(kinds) - 4):
        if kinds[i] != kinds[i - 1] and all(k == kinds[i] for k in kinds[i:i + 4]) and all(abs(i - c) >= 4 for c in cuts):
            cuts.append(i)
    cuts = sorted(cuts)
    cuts.append(len(files))
    result = []
    for a, b in zip(cuts, cuts[1:]):
        kind = max(set(kinds[a:b]), key=kinds[a:b].count)
        ground = classify(load(files[(a + b) // 2]))[1] if kind == 'race' else None
        if kind == 'race' and ground is None:
            ground = 'land'
        result.append({'start': a, 'end': b, 'kind': kind, 'ground': ground})
    # What the picture alone misreads (by the shot's first frame): the arm insert is too dark for
    # an insert and the start of it too flat, and DONT STOP's black and white reads as a race.
    for r in result:
        r['kind'] = OVERRIDES.get(r['start'], r['kind'])
        if r['kind'] != 'race':
            r['ground'] = None
    # Neighbouring titles and inserts are one piece each.
    merged = []
    for s in result:
        if merged and s['kind'] != 'race' and merged[-1]['kind'] == s['kind']:
            merged[-1]['end'] = s['end']
        else:
            merged.append(s)
    with open(out, 'w') as fh:
        json.dump({'fps': FPS, 'frames': len(files), 'shots': merged}, fh, indent=1)
    kinds = {}
    for s in merged:
        kinds[s['kind'] + ('/' + s['ground'] if s['ground'] else '')] = kinds.get(s['kind'] + ('/' + s['ground'] if s['ground'] else ''), 0) + 1
    print(f'{len(merged)} shots over {len(files) / FPS:.1f} s: {kinds}')


# The circuits each ground is filmed on, in turn: the intro shows the rivers and the dirt tracks.
CIRCUITS = {'water': ['WATER1', 'WATER2', 'WATER3'], 'land': ['DIRT1', 'DIRT2', 'DIRT3', 'RUF1', 'RUF2', 'CITY1']}
# The cameras, in turn (ABRRFilm): behind a car low, alongside it, a pack passing a fixed point,
# high behind, ahead looking back.
CAMERAS = ['chase', 'side', 'trackside', 'high', 'front']


def plan(shots_json, out):
    with open(shots_json) as fh:
        data = json.load(fh)
    turn = {k: 0 for k in CIRCUITS}
    cam = 0
    by_circuit = {}
    for i, s in enumerate(data['shots']):
        if s['kind'] != 'race':
            continue
        ground = s['ground'] or 'land'
        circuit = CIRCUITS[ground][turn[ground] % len(CIRCUITS[ground])]
        turn[ground] += 1
        s['circuit'] = circuit
        s['camera'] = CAMERAS[cam % len(CAMERAS)]
        s['racer'] = cam % 6
        s['id'] = i
        cam += 1
        by_circuit.setdefault(circuit, []).append(i)
    data['circuits'] = by_circuit
    with open(out, 'w') as fh:
        json.dump(data, fh, indent=1)
    print({c: len(v) for c, v in by_circuit.items()})


def assemble(plan_json, frames_dir, film_dir, audio, out):
    import imageio_ffmpeg
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    with open(plan_json) as fh:
        data = json.load(fh)
    work = os.path.join(film_dir, '_cut')
    os.makedirs(work, exist_ok=True)
    # One 30-a-second numbered sequence: each shot's frames for its length in the original.
    n = 0
    missing = 0
    for s in data['shots']:
        count = (s['end'] - s['start']) * 2  # 30 a second
        for k in range(count):
            if s['kind'] == 'race':
                src = os.path.join(film_dir, f"shot_{s['id']:03d}", f'{k:04d}.png')
            else:
                src = os.path.join(film_dir, 'up', f"frame_{s['start'] + k // 2:04d}.png")
            if not os.path.exists(src):
                missing += 1
                src = os.path.join(frames_dir, f"frame_{s['start'] + k // 2:04d}.png")
            dst = os.path.join(work, f'{n:05d}.png')
            if os.path.exists(dst):
                os.remove(dst)
            try:
                os.link(src, dst)
            except OSError:
                import shutil
                shutil.copyfile(src, dst)
            n += 1
    print(f'{n} frames, {missing} from the original (not filmed or upscaled)')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    cmd = [ffmpeg, '-y', '-framerate', '30', '-i', os.path.join(work, '%05d.png'), '-i', audio,
           '-vf', 'scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p',
           '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-c:a', 'aac', '-b:a', '192k', '-shortest', out]
    subprocess.run(cmd, check=True)
    print(out)


def homevideo(plan_json, frames_dir, model_path, out_dir):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import torch
    import upscale_textures as up
    with open(plan_json) as fh:
        data = json.load(fh)
    model = up.load_model(model_path)
    os.makedirs(out_dir, exist_ok=True)
    rng = np.random.default_rng(1996)
    n = 0
    for s in data['shots']:
        if s['kind'] != 'live':
            continue
        for f in range(s['start'], s['end']):
            # The codec's 2x2 and 4x4 blocks smoothed away first: a camcorder is soft, not blocky.
            from PIL import ImageFilter
            src = Image.open(os.path.join(frames_dir, f'frame_{f:04d}.png')).convert('RGB')
            src = src.filter(ImageFilter.MedianFilter(3)).filter(ImageFilter.GaussianBlur(0.9))
            with torch.no_grad():
                sharp = up.run(model, np.asarray(src)).astype(np.float32)
            plain = np.asarray(src.resize((sharp.shape[1], sharp.shape[0]), Image.BICUBIC), dtype=np.float32)
            # Half the model's detail: sharper than the tape, not a new picture.
            mixed = Image.fromarray(np.clip(plain * 0.45 + sharp * 0.55, 0, 255).astype(np.uint8)).resize((1920, 1200), Image.LANCZOS)
            a = np.asarray(mixed, dtype=np.float32)
            # The camera's grain: a fresh speckle each frame, coarser than a pixel, mostly in the light.
            grain = rng.normal(0, 1, (600, 960, 1)).astype(np.float32)
            grain = np.asarray(Image.fromarray(((grain[..., 0] * 40) + 128).clip(0, 255).astype(np.uint8)).resize((1920, 1200), Image.BILINEAR), dtype=np.float32)[..., None] - 128
            chroma = rng.normal(0, 3, (1, 1, 3)).astype(np.float32)  # a little colour flicker
            a = a + grain * (0.35 + 0.4 * a / 255.0) * 0.5 + chroma
            Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(os.path.join(out_dir, f'frame_{f:04d}.png'))
            n += 1
    print(f'{n} insert frames into {out_dir}')


def upscale_all(frames_dir, model_path, out_dir):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import torch
    import upscale_textures as up
    model = up.load_model(model_path)
    os.makedirs(out_dir, exist_ok=True)
    files = sorted(glob.glob(os.path.join(frames_dir, 'frame_*.png')))
    for f in files:
        with torch.no_grad():
            big = up.run(model, np.asarray(Image.open(f).convert('RGB')))
        Image.fromarray(big).resize((1920, 1200), Image.LANCZOS).save(os.path.join(out_dir, os.path.basename(f)))
    print(f'{len(files)} frames into {out_dir}')


def encode(frames_dir, fps, audio, out):
    import imageio_ffmpeg
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-y', '-framerate', str(fps), '-i', os.path.join(frames_dir, 'frame_%04d.png'), '-i', audio,
                    '-vf', 'scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p',
                    '-r', '30', '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-c:a', 'aac', '-b:a', '192k', out], check=True)
    print(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('shots'); a.add_argument('frames'); a.add_argument('out')
    a = sub.add_parser('plan'); a.add_argument('shots'); a.add_argument('out')
    a = sub.add_parser('upscale'); a.add_argument('frames'); a.add_argument('model'); a.add_argument('out')
    a = sub.add_parser('encode'); a.add_argument('frames'); a.add_argument('fps'); a.add_argument('audio'); a.add_argument('out')
    a = sub.add_parser('homevideo'); a.add_argument('plan'); a.add_argument('frames'); a.add_argument('model'); a.add_argument('out')
    a = sub.add_parser('assemble'); a.add_argument('plan'); a.add_argument('frames'); a.add_argument('film'); a.add_argument('audio'); a.add_argument('out')
    args = p.parse_args()
    if args.cmd == 'shots':
        shots(args.frames, args.out)
    elif args.cmd == 'plan':
        plan(args.shots, args.out)
    elif args.cmd == 'upscale':
        upscale_all(args.frames, args.model, args.out)
    elif args.cmd == 'encode':
        encode(args.frames, args.fps, args.audio, args.out)
    elif args.cmd == 'homevideo':
        homevideo(args.plan, args.frames, args.model, args.out)
    else:
        assemble(args.plan, args.frames, args.film, args.audio, args.out)


if __name__ == '__main__':
    main()
