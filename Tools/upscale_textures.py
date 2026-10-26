"""Upscales the original's texture pages 4x with an ESRGAN-family model, for the remastered look.

Usage: python -I Tools/upscale_textures.py <model.pth> <in.png> <out.png> [--tile 32]
       python -I Tools/upscale_textures.py <model.pth> --batch <Saved/BRRAtlases> <cache dir>

--batch takes every page the importer wrote (BRRAtlases/terrain/<hash>.png, tiled by 32, and
BRRAtlases/pages/<hash>.png) that the cache does not hold yet: the model's output is kept as
<hash>_raw.png, and from it and the page come <hash>.png (the colour, see tame) and <hash>_nr.png (normal and
roughness, see detail_map), which the next import uses. --refinish remakes those two from the raw
upscales, without the model, after changing either step.
The project uses 4x_foolhardy_Remacri: of the models tried on the original's art it adds the most
texture without speckling it (README).

Run it with the venv in Tools/upscale (git-ignored; torch, spandrel, pillow, numpy - see README).
Models are community ESRGAN weights (OpenModelDB); they are loaded with torch.load(weights_only=True),
so a downloaded .pth can hold tensors only, never code.

--tile N treats the page as a grid of NxN terrain tiles: each is upscaled on its own, padded by
mirroring it, because the map lays tiles next to unrelated ones (and mirrors and turns them) - the
page's own neighbours are not the ones it will meet. Without it the page is upscaled whole.

Transparency (the palette's index 0, alpha 0) does not go through the model: the transparent texels
first take their nearest opaque neighbour's colour, so no magenta or black bleeds into the edges,
and the alpha is scaled smoothly and cut back to on/off at one half - the masked materials draw it
that way.
"""
import argparse
import glob
import os

import numpy as np
import spandrel
import torch
from PIL import Image

SCALE = 4
PAD = 8  # source texels of context round each piece


def load_model(path):
    state = torch.load(path, map_location="cpu", weights_only=True)
    for key in ("params_ema", "params", "state_dict"):
        if isinstance(state, dict) and key in state and isinstance(state[key], dict):
            state = state[key]
    model = spandrel.ModelLoader().load_from_state_dict(state)
    if model.scale != SCALE:
        raise SystemExit(f"{path}: a {model.scale}x model, wanted {SCALE}x")
    return model.eval()


def fill_transparent(rgb, alpha):
    """Every transparent texel takes the colour of the nearest opaque one (grown ring by ring)."""
    rgb = rgb.copy()
    known = alpha > 0
    if known.all() or not known.any():
        return rgb
    while not known.all():
        grown = known.copy()
        acc = np.zeros_like(rgb, dtype=np.float64)
        cnt = np.zeros(known.shape, dtype=np.float64)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), axis=(0, 1))
            c = np.roll(rgb, (dy, dx), axis=(0, 1))
            acc += c * k[..., None]
            cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = (acc[new] / cnt[new][:, None]).astype(rgb.dtype)
        grown |= new
        known = grown
    return rgb


@torch.inference_mode()
def run(model, rgb):
    """rgb: HxWx3 uint8 -> (4H)x(4W)x3 uint8, with PAD texels of mirrored context."""
    padded = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="reflect")
    t = torch.from_numpy(padded).float().div(255).permute(2, 0, 1).unsqueeze(0)
    out = model(t).clamp(0, 1).squeeze(0).permute(1, 2, 0).mul(255).round().byte().numpy()
    p = PAD * SCALE
    return out[p:-p, p:-p]


def upscale(model, image, tile):
    rgba = np.asarray(image.convert("RGBA"))
    rgb, alpha = rgba[..., :3], rgba[..., 3]
    rgb = fill_transparent(rgb, alpha)
    h, w = alpha.shape
    out = np.zeros((h * SCALE, w * SCALE, 3), np.uint8)
    if tile:
        for y in range(0, h, tile):
            for x in range(0, w, tile):
                out[y * SCALE:(y + tile) * SCALE, x * SCALE:(x + tile) * SCALE] = run(model, rgb[y:y + tile, x:x + tile])
    else:
        out = run(model, rgb)
    big_alpha = np.asarray(Image.fromarray(alpha).resize((w * SCALE, h * SCALE), Image.BICUBIC))
    big_alpha = np.where(big_alpha >= 128, 255, 0).astype(np.uint8)
    return Image.fromarray(np.dstack([out, big_alpha]), "RGBA")


def blur(a, sigma):
    """Separable gaussian, edges mirrored."""
    r = max(1, int(sigma * 3))
    x = np.arange(-r, r + 1)
    k = np.exp(-x * x / (2 * sigma * sigma))
    k /= k.sum()
    for axis in (0, 1):
        p = np.pad(a, [(r, r) if i == axis else (0, 0) for i in range(2)], mode="reflect")
        a = sum(w * np.take(p, np.arange(i, i + a.shape[axis]), axis=axis) for i, w in enumerate(k))
    return a


def tame(raw, source, cap=16.0, lo=6.0, hi=14.0):
    """Holds the upscale to what the original says. Remacri reads the original's faint dithering
    (the track dirt is rows of one-texel dashes) as line art and draws crisp light scratches; across
    a sunlit track, seen low, they were all one saw. So: where the source is low-contrast (local
    deviation under `lo` of 255) the page is the plain smooth enlargement, where it has structure
    (over `hi`: cobbles, rock edges, grass clumps, lettering) Remacri's detail, and between them a
    blend; and that detail never strays more than `cap` from the smooth enlargement."""
    big = np.asarray(raw.convert("RGBA"))
    src = np.asarray(source.convert("RGBA"))
    h, w = src.shape[:2]
    filled = fill_transparent(src[..., :3], src[..., 3])
    ref = np.asarray(Image.fromarray(filled).resize((w * SCALE, h * SCALE), Image.BICUBIC)).astype(np.float64)
    lum = filled.astype(np.float64) @ np.array([0.2126, 0.7152, 0.0722])
    mean = blur(lum, 1.5)
    dev = np.sqrt(np.maximum(blur(lum * lum, 1.5) - mean * mean, 0))
    weight = np.clip((dev - lo) / (hi - lo), 0, 1)
    weight = np.asarray(Image.fromarray(weight.astype(np.float32), "F").resize((w * SCALE, h * SCALE), Image.BILINEAR))[..., None]
    guided = ref + np.clip(big[..., :3] - ref, -cap, cap)
    rgb = np.clip(ref + weight * (guided - ref), 0, 255).round().astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, big[..., 3]]), "RGBA")


def detail_piece(lum):
    """A piece's normal (RGB, tangent space, +Y down the image as UE's) and roughness variation (A)."""
    height = blur(lum, 1.5)
    gy, gx = np.gradient(height)
    strength = 2.5
    n = np.dstack([-gx * strength, -gy * strength, np.ones_like(height)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    # Darker than its surroundings: a crevice, rougher. Never smoother than the surface's own
    # roughness: the light flecks made smoother glinted all over a sunlit dirt track.
    rough = 0.5 + np.clip((blur(lum, 6.0) - lum) * 1.5, 0.0, 0.25)
    return np.dstack([(n * 0.5 + 0.5), rough])


def detail_map(image, tile):
    """The upscaled page's normal/roughness map, tile by tile for terrain pages."""
    rgba = np.asarray(image.convert("RGBA")).astype(np.float64) / 255
    lum = rgba[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    h, w = lum.shape
    out = np.zeros((h, w, 4))
    step = tile * SCALE if tile else max(h, w)
    for y in range(0, h, step):
        for x in range(0, w, step):
            out[y:y + step, x:x + step] = detail_piece(lum[y:y + step, x:x + step])
    out[rgba[..., 3] == 0] = (0.5, 0.5, 1.0, 0.5)
    return Image.fromarray((out * 255).round().astype(np.uint8), "RGBA")


def batch(model_path, dump, cache, refinish):
    os.makedirs(cache, exist_ok=True)
    jobs = [(f, 32) for f in sorted(glob.glob(os.path.join(dump, "terrain", "*.png")))]
    jobs += [(f, 0) for f in sorted(glob.glob(os.path.join(dump, "pages", "*.png")))]
    stem = lambda f: os.path.join(cache, os.path.basename(f)[:-4])
    todo = [(f, t) for f, t in jobs if refinish or not os.path.exists(stem(f) + "_nr.png")]
    print(f"{len(jobs)} pages, {len(todo)} to do", flush=True)
    model = None
    for i, (f, tile) in enumerate(todo):
        if os.path.exists(stem(f) + "_raw.png"):
            raw = Image.open(stem(f) + "_raw.png")
        else:
            model = model or load_model(model_path)
            raw = upscale(model, Image.open(f), tile)
            raw.save(stem(f) + "_raw.png")
        colour = tame(raw, Image.open(f))
        colour.save(stem(f) + ".png")
        detail_map(colour, tile).save(stem(f) + "_nr.png")
        print(f"{i + 1}/{len(todo)} {os.path.basename(f)}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--tile", type=int, default=0)
    ap.add_argument("--batch", action="store_true", help="input is Saved/BRRAtlases, output the cache dir")
    ap.add_argument("--refinish", action="store_true", help="--batch: remake every page from its raw upscale")
    a = ap.parse_args()
    if a.batch:
        batch(a.model, a.input, a.output, a.refinish)
    else:
        upscale(load_model(a.model), Image.open(a.input), a.tile).save(a.output)


if __name__ == "__main__":
    main()
