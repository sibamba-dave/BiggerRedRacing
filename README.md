# Bigger Red Racing

A UE 5.8 rebuild of *Big Red Racing* (Big Red Software / Domark, 1996). Phase A is a faithful rebuild; Phase B modernises it (Lumen/HW ray tracing, DLSS, upgraded assets).

**No original game data is included or distributed.** Point the project at your own copy of the game's `RACING` folder:
- set `BRR_DATA_DIR`, or
- edit `[BRR] DataDir` in `Game/Config/DefaultGame.ini` (default: `../Big Red Racing - RIP/RACING`).

## Layout
- `Game/` – UE 5.8 C++ project
  - `Source/BRRFormats` – parsers for the original file formats (runtime, no engine deps beyond Core)
  - `Source/BRRGame` – gameplay: the race game mode (`ABRRRaceGameMode`, default for every map),
    the player's Chaos car fitted to the original's handling (`ABRRVehiclePawn`, see
    `Docs/Handling.md`), ghost opponents (`ABRRGhostCar`) and an interim text HUD
  - `Plugins/BRRImporter` – editor plugin that bakes the original data into `Content/BRR/<LEVEL>/`
    (textures, unlit materials, terrain/decor/vehicle meshes, a map with the grid). Local only: the
    output is derived from your copy of the game and is gitignored.
- `ThirdParty/OpenRedRacing` – reference reimplementation (submodule), used as a format/physics oracle
- `Docs/Formats.md` – format spec

## Build & test
```
D:\Games\UE_5.8\Engine\Build\BatchFiles\Build.bat BiggerRedRacingEditor Win64 Development -Project=%CD%\Game\BiggerRedRacing.uproject
UnrealEditor-Cmd.exe Game\BiggerRedRacing.uproject -ExecCmds="Automation RunTests BRR;Quit" -unattended -NullRHI
```

## Importing the original levels
In the editor: **Tools → Big Red Racing → <LEVEL>** (or *Import all levels*). Headless:
```
UnrealEditor-Cmd.exe Game\BiggerRedRacing.uproject -run=BRRImport -Level=DIRT1 [-Vehicles=JEEP,ATV] [-Colour=1]
UnrealEditor-Cmd.exe Game\BiggerRedRacing.uproject -run=BRRImport -All
```
Then open `/Game/BRR/<LEVEL>/L_<LEVEL>`.

Screenshot regression (no window; run from PowerShell so `/Game/...` isn't path-mangled):
```
UnrealEditor.exe Game\BiggerRedRacing.uproject /Game/BRR/DIRT1/L_DIRT1 -game -RenderOffScreen -ResX=1280 -ResY=800 -BRRShot=60 -BRRShotName=DIRT1.png
```
writes `Game/Saved/Screenshots/WindowsEditor/DIRT1.png` and quits.

## Racing
Import a level, open `/Game/BRR/<LEVEL>/L_<LEVEL>` and press Play (or run with `-game`). You drive
the level's first car (`?Vehicle=ATV` to change) from a grid block chosen as the original does,
against the level's ghost laps, under the original's rules: countdown, CHECKP lines, laps, placing,
wrong way, rubber-banded ghosts.

Controls: W/Up accelerate, S/Down brake and reverse, A/D or Left/Right steer, Space handbrake,
R recover, Shift boost. Gamepad: triggers, left stick, X handbrake, Y recover, A boost.

Dev switches: `-BRRDriveTest` (handling comparison, see `Docs/Handling.md`), `-BRRAutopilot` (drives
the player's car along a ghost's racing line, recovering when stuck).

View distance: `brr.ViewCells` (console or `-ExecCmds="brr.ViewCells N"`) draws the world N terrain
cells deep, as the original did; it retuned this between 10 and 22 against the frame rate, and the
default is 16. 0 draws the whole map.

Surface handling: the player's car takes the original's handling for the surface under it (track,
slippy, grass, sand, ...: turning, acceleration, braking, top speed). `brr.SurfaceHandling 0` uses
the track's everywhere.

Palette check: `python Tools/palette_check.py <screenshot.png> <palette.png>` reports pixels that are
not the level's palette colours (export `T_<LEVEL>_Palette` from the import as the palette).

## Classic and remastered looks
`brr.Look` picks the look, live (F5 in a race):
- `0`, classic: the original exactly - unlit palette colours (every pixel a palette entry,
  `Tools/palette_check.py`), its banded sky, its view distance, 4:3, no anti-aliasing.
- `1`, remastered (default): the same world lit - the level's own sun in a sky atmosphere with
  volumetric clouds and height fog, Lumen global illumination and reflections on hardware ray
  tracing, virtual shadow maps, filmic post (auto exposure, bloom, AO, motion blur), filtered and
  mipmapped textures, widescreen, everything in view, upscaled by TSR (or DLSS when NVIDIA's DLSS
  plugin is installed in the engine: it is picked up automatically). `brr.PathTracing 1` renders
  through the path tracer (with the engine's denoiser) for stills.

The materials carry both looks (`MPC_BRR.Remaster`); `ABRRLook` spawns the remastered sky and
lighting and sets the renderer. Needs DX12 / SM6 and a ray-tracing GPU for the remastered look.

## Reference captures of the original
The original runs in DOSBox-X for side-by-side checks. Put the portable 64-bit build in
`Tools/dosbox-x/` (git-ignored), copy the rip's `RACING\` to `Tools/dosbox-x/c/RACING\`, and start it
with `Tools/brr-dosbox.conf` (fix its paths). `RACING.EXE` takes `-CFG <name>` (skips name entry),
`-NOSOUND` and `-LEV <LEVEL>`, which goes straight into a single race on that level (`-CAR` crashed
here).

`python Tools/brr_cd.py <BRR_CD.img> <out_dir> --rip <rip_dir>` reads a raw CD image (2352-byte
sectors, Mode 2 data track): it copies any CD file the rip lacks into the rip, writes the audio
tracks as WAVs and writes the cue sheet DOSBox-X mounts the image with (CD music included). The
music and the intro video (`VIDEO/INTROSND.RPL`, Escape 122) are the original's: keep them out of git.

## Credits
Format parsers are adapted from [OpenRedRacing](https://github.com/nynjardin/OpenRedRacing) (MIT, Copyright (c) 2026 PaPe), which in turn credits jacobgelling/red-archive and red-image. Big Red Racing and its data are the property of their respective owners.
