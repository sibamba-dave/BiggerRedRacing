"""Vendor OpenRedRacing's format parsers and simulation into the BRRFormats UE module (private; BRRFormats is the only boundary to it).

Copies ThirdParty/OpenRedRacing/src/{formats,sim} plus the CPU-only scene builders (LevelScene,
vehicle meshes) verbatim into Game/Source/ORRCore/Public so the
original include layout ("formats/x.h", "sim/x.h") keeps working, and records the upstream commit.
Re-run after bumping the submodule; never hand-edit the vendored files (patch upstream or wrap them
in BRRFormats instead).

Usage: python Tools/sync_orr.py
"""
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "ThirdParty" / "OpenRedRacing" / "src"
DST = ROOT / "Game" / "Source" / "BRRFormats" / "Private" / "ORR"
DIRS = ["formats", "sim"]
# Mesh builders only: no GL/SDL. They turn the original data into the triangle lists ORR draws.
SCENE_FILES = ["level_scene.h", "level_scene.cpp", "vehicle_render.h", "vehicle_render.cpp"]


def main() -> None:
    for d in DIRS:
        target = DST / d
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(SRC / d, target)
    scene = DST / "scene"
    if scene.exists():
        shutil.rmtree(scene)
    scene.mkdir()
    for f in SCENE_FILES:
        shutil.copy(SRC / "scene" / f, scene / f)
    commit = subprocess.run(["git", "-C", str(SRC.parent), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=True).stdout.strip()
    (DST / "ORR_VERSION.txt").write_text(
        f"OpenRedRacing {commit}\nhttps://github.com/nynjardin/OpenRedRacing\n"
        f"Vendored: {', '.join('src/' + d for d in DIRS)}, src/scene/{{{','.join(SCENE_FILES)}}} (MIT, Copyright (c) 2026 PaPe)\n")
    shutil.copy(SRC.parent / "LICENSE", DST / "LICENSE.OpenRedRacing")
    print(f"Vendored ORR {commit[:10]} into {DST}")


if __name__ == "__main__":
    main()
