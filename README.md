# Bigger Red Racing

A UE 5.8 rebuild of *Big Red Racing* (Big Red Software / Domark, 1996). Phase A is a faithful rebuild; Phase B modernises it (Lumen/HW ray tracing, DLSS, upgraded assets).

**No original game data is included or distributed.** Point the project at your own copy of the game's `RACING` folder:
- set `BRR_DATA_DIR`, or
- edit `[BRR] DataDir` in `Game/Config/DefaultGame.ini` (default: `../Big Red Racing - RIP/RACING`).

## Layout
- `Game/` – UE 5.8 C++ project
  - `Source/BRRFormats` – parsers for the original file formats (runtime, no engine deps beyond Core)
  - `Source/BRRGame` – gameplay
- `ThirdParty/OpenRedRacing` – reference reimplementation (submodule), used as a format/physics oracle
- `Docs/Formats.md` – format spec

## Build & test
```
D:\Games\UE_5.8\Engine\Build\BatchFiles\Build.bat BiggerRedRacingEditor Win64 Development -Project=%CD%\Game\BiggerRedRacing.uproject
UnrealEditor-Cmd.exe Game\BiggerRedRacing.uproject -ExecCmds="Automation RunTests BRR;Quit" -unattended -NullRHI
```

## Credits
Format parsers are adapted from [OpenRedRacing](https://github.com/nynjardin/OpenRedRacing) (MIT, Copyright (c) 2026 PaPe), which in turn credits jacobgelling/red-archive and red-image. Big Red Racing and its data are the property of their respective owners.
