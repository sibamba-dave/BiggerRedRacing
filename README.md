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
R recover. Gamepad: triggers, left stick, X handbrake, Y recover.

Dev switches: `-BRRDriveTest` (handling comparison, see `Docs/Handling.md`), `-BRRAutopilot` (drives
the player's car along a ghost's racing line, recovering when stuck).

## Credits
Format parsers are adapted from [OpenRedRacing](https://github.com/nynjardin/OpenRedRacing) (MIT, Copyright (c) 2026 PaPe), which in turn credits jacobgelling/red-archive and red-image. Big Red Racing and its data are the property of their respective owners.
