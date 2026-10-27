"""Fetches the remastered look's ground materials (CC0, ambientCG) and packs them for the importer.

    Tools\\upscale\\.venv\\Scripts\\python -I Tools\\fetch_ground.py

Each surface kind becomes Tools/ground/<kind>.png, 2048 square, RGBA:
  R, G  its tangent-space normal's X and Y (DirectX convention, as UE's), 0..1
  B     its roughness
  A     its brightness, held to a mean of one half: the fine detail the remastered terrain lays
        over the original's colour (which stays the original's: the detail only modulates it)
and Tools/ground/<kind>_albedo.png, 2048 square, RGB: the photo's colour divided by its own mean
colour and halved (0.5 = the mean), so the importer's terrain can wear the photo's grain and hue
variation over the original art's colour (its tint) without taking the photo's palette.
The downloads go to Tools/ground/download/ (git-ignored, with the packed pages). Kinds, in the
order the importer's texture array holds them (BRRImporter: GroundKinds):
"""

import io
import sys
import urllib.request
import zipfile
from pathlib import Path

from PIL import Image
import numpy as np

KINDS = [
    ("dirt", "Ground109"),     # dry, beige-brown dirt
    ("grass", "Grass004"),     # dense grass
    ("sand", "Ground080"),     # beach sand
    ("snow", "Snow010A"),      # clean snow
    ("rock", "Rock058"),       # grey cliff
    ("asphalt", "Asphalt031"), # light grey asphalt
    ("regolith", "Ground110"), # dry grey gravel: the moon's dust
]
SIZE = 2048
RES = "2K"


def fetch(asset_id: str, dest: Path) -> Path:
    folder = dest / f"{asset_id}_{RES}"
    if folder.exists() and any(folder.iterdir()):
        return folder
    folder.mkdir(parents=True, exist_ok=True)
    url = f"https://ambientcg.com/get?file={asset_id}_{RES}-JPG.zip"
    print(f"  downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "BiggerRedRacing-fetch_ground"})
    with urllib.request.urlopen(req) as r:
        data = r.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name in z.namelist():
            # Only the maps used, by their plain file names (no paths from the archive).
            base = Path(name).name
            if base.endswith(".jpg") and any(t in base for t in ("_Color", "_NormalDX", "_Roughness")):
                (folder / base).write_bytes(z.read(name))
    return folder


def load(folder: Path, tag: str, mode: str) -> np.ndarray:
    found = sorted(folder.glob(f"*_{tag}.jpg"))
    if not found:
        raise SystemExit(f"{folder.name}: no {tag} map")
    img = Image.open(found[0]).convert(mode).resize((SIZE, SIZE), Image.LANCZOS)
    return np.asarray(img, dtype=np.float32) / 255.0


def pack(folder: Path) -> Image.Image:
    colour = load(folder, "Color", "RGB")
    normal = load(folder, "NormalDX", "RGB")
    rough = load(folder, "Roughness", "L")
    luma = colour @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    detail = np.clip(luma * (0.5 / max(float(luma.mean()), 1e-3)), 0.0, 1.0)
    out = np.dstack([normal[..., 0], normal[..., 1], rough, detail])
    return Image.fromarray((out * 255.0 + 0.5).astype(np.uint8), "RGBA")


def albedo(folder: Path) -> Image.Image:
    """The photo's colour relative to its own mean, halved: 0.5 is the mean (see the docstring)."""
    colour = load(folder, "Color", "RGB")
    # In linear light, so the ratio multiplies a linear tint correctly.
    lin = np.where(colour <= 0.04045, colour / 12.92, ((colour + 0.055) / 1.055) ** 2.4)
    ratio = lin / np.maximum(lin.reshape(-1, 3).mean(axis=0), 1e-3)
    out = np.clip(ratio * 0.5, 0.0, 1.0)
    return Image.fromarray((out * 255.0 + 0.5).astype(np.uint8), "RGB")


def main() -> int:
    root = Path(__file__).resolve().parent / "ground"
    download = root / "download"
    download.mkdir(parents=True, exist_ok=True)
    for kind, asset_id in KINDS:
        print(f"{kind}: {asset_id}")
        folder = fetch(asset_id, download)
        pack(folder).save(root / f"{kind}.png")
        albedo(folder).save(root / f"{kind}_albedo.png")
    print(f"packed {len(KINDS)} kinds into {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
