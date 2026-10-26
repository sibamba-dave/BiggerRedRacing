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

## Front end
Run the game (`-game`, no map) and it starts on the original's menus: single race, the
championship, options. Each screen is the original's own `.SCN` (picture, sprites, the wibbly
WIBS lettering and the strings of `TOURN.<LANG>` where its script puts them, its click boxes and
shortcut keys - `BRR::LoadScn`, `FBRRText`), drawn at 320x200 over a 4:3 picture with the classic
look, so its pixels are the palette's. The flow is the original's: CONFSCN, GLOBESCN (the circuit:
its picture stands where the original's 3D globe turns), TRAKSCN (car, preview lap, race; the
circuit's music starts here), CARSCN (the vehicle turning in front of its picture, its stats),
the race, RESULSCN. The championship is TOURN.TXT's seven rounds: win the round's `need` of its
circuits to go on (ORR's rules; for the session only, as in the original). Options: sound
(music and effects volumes, commentary) and display (the classic view distance, and the look).
The intro (`VIDEO/INTROSND.RPL`, beside `RACING`) plays first, the victory video (`VICTORY.MVI`)
after a championship won; any key skips them, `-nointro` leaves the intro out. Both are decoded as
they play (`FBRRVideo`, from `Tools/brr_video.py`), the intro at the 15 frames a second its sound
keeps. Escape leaves a race for the menus; a preview lap is driven by the autopilot. Settings are saved
in `GameUserSettings.ini` `[BRR]`. `-BRRMenuWalk=0,0,3` presses those boxes a second and a half
apart, shooting each screen (`Menu_<n>.png`), then quits; `99` waits, `-1` is Escape, `-2` right, `-3` a sample
results screen, `-4`/`-5` the intro/victory video.

## Racing
Import a level, open `/Game/BRR/<LEVEL>/L_<LEVEL>` and press Play (or run with `-game`). You drive
the level's first car (`?Vehicle=ATV` to change) from a grid block chosen as the original does,
against the level's ghost laps, under the original's rules: countdown, CHECKP lines, laps, placing,
wrong way, rubber-banded ghosts. Every car stays where the grid put it until the lights go out.
The ghosts are not solid to the player's physics: running into one (or being run into) is the
original's car-to-car contact - pushed out, half the closing speed back, a small spin.

The game runs in a borderless window over the whole desktop (Alt+Enter for a window; `-windowed` or
`-ResX=`/`-ResY=` to start in one).

Controls: W/Up accelerate, S/Down brake and reverse, A/D or Left/Right steer, Space handbrake,
R recover, Shift boost, P photo, F5 look, C camera (the three chase heights, then the cabin: the
vehicle's interior with its own dials, redrawn live by the original's `ndial` gauges). Gamepad: triggers, left stick, X handbrake, Y recover, A boost.

Sound: the original's own - every car's engine (its pitch on the gear table's revs), skids and
rolling on each surface, impacts, splats, landings, the boost, the countdown tones, checkpoint and
lap cues, and the circuit's commentator - mixed by ORR's rules (eight voices, three engines, the
commentator ducking the rest; `BRRAudio`, `FBRRRaceSound`). Under it the CD music: each circuit's
intro track then its loop, from TOURN.TXT, played from the rip's `Music/TrackNN.wav`
(`Tools/brr_cd.py`; `[BRR] MusicDir` to point elsewhere). `brr.SoundVolume`, `brr.MusicVolume`,
`brr.Commentary`. `-BRRAudioDump=<file.wav>` records 20 seconds of the output.

Dev switches: `-BRRCabin` (start in the cabin view), `-BRRDriveTest` (handling comparison, see `Docs/Handling.md`), `-BRRAutopilot` (drives
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
  mipmapped textures, widescreen, everything in view, rendered at two thirds and upscaled: by DLSS
  Super Resolution with Ray Reconstruction (denoising Lumen's rays), plus DLSS Frame Generation and
  Reflex, when NVIDIA's plugins are installed (`brr.FrameGeneration 0` turns frame generation off),
  else by TSR. `brr.DLAA 1` renders at full resolution instead: DLAA (TSR as plain anti-aliasing
  without DLSS). The terrain's liquids are shaded as what they are: water dark and glassy under
  moving ripples, reflecting the sky and the banks; ice smooth and shining; lava glowing. The
  original has no such notion, so `FBRRWorld::LiquidOf` reads it off each level's surface types:
  the WATER circuits' river is TRACK, the lakes and the sea elsewhere are SLIPPY, which is ice on
  SNOW and lava on MOON. The vehicles are glossy paint (bodies) and dull rubber (wheels).
  The chase camera is the original's rope, hung closer and higher: 0.6 of its distance behind
  (`brr.CameraDistance`) and raised by a fifth of that (`brr.CameraLift`), still aimed where the
  original aims; both ease off in tunnels. The original's own, at 34 m behind and 3.5 m up at
  speed, skims the ground on a wide screen. `brr.LogPerf 1` logs the frame rate and the race's
  pace against the wall clock.

NVIDIA's DLSS 4.5 plugins for UE 5.8 (DLSS, Streamline and its parts) go in the engine, e.g.
`Engine/Plugins/Marketplace/NVIDIA/`; the project enables `DLSS` and `Streamline`. They are not part
of this repository.

Photo mode (P in a race): the race pauses, the path tracer gathers `brr.PhotoSamples` (512) samples
per pixel at full resolution, the engine's denoiser cleans them, and the still is saved without the
HUD to `Saved/Screenshots/` as `Photo_<LEVEL>_<time>.png`; then the race carries on. The path tracer
starts its samples over whenever anything moves, and UE 5.8's temporal denoiser for it is
offline-only, so it is for stills; `brr.PathTracing 1` shows it live, grainy in motion.
`-BRRPhoto=<seconds>` takes one at that point in the race and quits.

The materials carry both looks (`MPC_BRR.Remaster`); `ABRRLook` spawns the remastered sky and
lighting and sets the renderer. Needs DX12 / SM6 and a ray-tracing GPU for the remastered look.

### Upscaled textures
The remastered look draws the original's texture pages upscaled 4x by an ESRGAN model, with a
normal and roughness map derived from each; classic keeps the original pixels. The import writes
every page to `Game/Saved/BRRAtlases/<terrain|pages>/<hash>.png` (named by its pixels, so a page
levels share is done once), `Tools/upscale_textures.py --batch` upscales what the cache lacks into
`Tools/upscale/cache/`, and the next import uses it:
```
py -3.11 -m venv Tools\upscale\.venv
Tools\upscale\.venv\Scripts\python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
Tools\upscale\.venv\Scripts\python -m pip install spandrel pillow numpy
Tools\upscale\.venv\Scripts\python -I Tools\upscale_textures.py Tools\upscale\models\4x_foolhardy_Remacri.pth --batch Game\Saved\BRRAtlases Tools\upscale\cache
```
The model is `4x_foolhardy_Remacri.pth` (OpenModelDB; e.g. huggingface.co/uwg/upscaler), in
`Tools/upscale/models/` (git-ignored, like the venv and the cache). Of those tried on the
original's art (Real-ESRGAN x4plus, UltraSharp, NMKD Siax, NMKD Superscale, Remacri) it adds the
most texture without speckling it; x4plus, trained on photographs, smears the ground flat. Terrain
pages are upscaled tile by tile (the map lays tiles next to unrelated ones), and transparency
stays crisp. The raw upscale (`<hash>_raw.png`) is then held to the original: Remacri reads faint
dithering (the track dirt) as line art and draws scratches, so low-contrast areas take the smooth
enlargement and only structure (cobbles, rock edges, lettering) keeps the model's detail;
`--refinish` redoes that step, and the normal/roughness maps, without the model. Grass and dirt get
no real detail this way - there is none in the source for a model to find.

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
