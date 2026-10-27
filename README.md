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
    `Docs/Handling.md`; a boat, hovercraft or helicopter, with nothing that rolls, runs on the
    original's own physics instead, `FBRRRace::TickSimulated`), ghost opponents (`ABRRGhostCar`) and an interim text HUD
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
(music and effects volumes, commentary) and graphics: the look, borderless / fullscreen /
windowed (borderless by default), resolution, vsync, frame limit, quality (UE's scalability; Lumen
needs High or above), upscaling (DLSS/TSR quality, or native), frame generation, motion blur, HDR
and the classic view distance.
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

Ground effects: every car raises the original's dust, spray and impact sparks (the theme's `spot`
shapes, ORR's 100-puff `ParticleField` playing each flipbook where it was born), drawn each frame
from the import's atlas pages (`ABRRParticles`).

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

### Ground materials
That detail comes instead from CC0 PBR materials (ambientCG): dirt, grass, sand, snow, rock,
asphalt and the moon's dust. `Tools/fetch_ground.py` downloads them and packs each into
`Tools/ground/<kind>.png` (git-ignored): its normal, its roughness, and its brightness held to a
mean of one half. The import makes them one texture array (`T_BRR_Ground`) and gives every terrain
triangle the kinds at its three corners (`FBRRWorld::GroundOf` per triangle, then each corner the
kind most of the ground around it is, rounded off twice; UV1-3), which the material blends across
the triangle by weight and by each material's height, so the kinds meet along curves. The surface types are handling, not looks,
and the palette art is stylised, so the level's theme decides first (the moon is dust, SNOW
snow), then the colour the original paints the triangle (green grass, grey made ground or rock,
the rest earth). The remastered look lays the kind's material in the world every 3 m: its
brightness modulates the original's colour (which stays the original's), and its relief and
roughness join the page's own. It fades out between 40 and 160 m, where the tiling would show.
The classic look is untouched. Without the packed files the import still runs, with no detail.
```
Tools\upscale\.venv\Scripts\python -I Tools\fetch_ground.py
```

### Refined geometry
The remastered look draws more than the original's polygons, from the same data
(`BRRRefine.h`). None of it collides: the race, Chaos and the wheels keep the original's geometry.
The classic look keeps it all exactly (`ABRRLook` shows `BRRRemasterOnly` actors and hides
`BRRClassicOnly` ones, which stay solid, and swaps a drawn-only mesh for its `_HD` version).
- Smooth shading everywhere: up to 50° between faces on the terrain, 35° on the scenery and 25° on
  the bodies; sharper edges stay sharp. (Normals change nothing in the unlit classic look.)
- The terrain (`SM_<L>_TerrainHD`): each triangle cut 4 x 4 onto a Catmull-Rom surface through the
  physics' own heights at the cell corners (`FBRRWorld::RefinedOffsetAt`). It meets the original at
  every corner, curves between them by up to 60 cm, and stays exactly the original on the track,
  the ramps and the liquids, so wheels, decals and water sit where they did. Nanite tessellates it,
  and the ground materials displace it by a few centimetres (`M_BRR_TexturedHD`).
- Water (`SM_<L>_WaterHD`): the open water is UE's single layer water in the level's own water colour.
  Its bed is the refined terrain itself, let down (`FBRRWorld::ShoreSinkAt`, Catmull-Rom between the
  corners) by 80 cm at the shore and 1.5 m more a cell out, to 4.5 m; the water reaches a cell over
  the banks at its level, so the shore is where the smooth banks cross it, not the original's
  triangle edges. The dry track and the ramps keep their height. Ice and lava are still the terrain's.
- Round wheels (`SM_<V>_Wheel<n>_HD`): the six- or eight-sided tyres subdivided and put back on
  their circle.
- Ground detail (`ABRRScatter`): grass tufts, pebbles and rocks within 25 m of the track, coloured
  by the ground under each, placed when the race starts (a few hundred thousand, in well under a
  tenth of a second); procedural shapes from the import (`SM_BRR_Grass<n>`, `SM_BRR_Stone<n>`).
- Wakes (`ABRRWake`, `M_BRR_Foam`): every boat on open water leaves churned water behind it and two
  arms spreading at the Kelvin angle, throws spray off its bow at speed, and splashes where an arm
  runs into the bank. Dust (`ABRRDust`, `M_BRR_Haze`): a car on dirt, sand or moon dust leaves pale
  clouds that spread, drift and thin out over a few seconds. The original's own puffs and splashes
  (`M_BRR_Effect`) are unlit at a sunlit brightness and stand on the surface instead of half under it.
- Interface art (`BRRArt`, `BRR::SmoothPixelArt`): the fonts and the HUD's sprites are traced (each
  colour's outline, one-pixel steps cut into slopes, square corners kept) and redrawn at up to 8x;
  pictures keep their pixels. `brr.SmoothArt 0` keeps the original's pixels in this look too.

### The remastered intro
`Tools/brr_film.py` rebuilds the intro: the original's cut, its race shots filmed again in the
engine. `shots` finds the cuts in the decoded intro and sorts them into race shots (on water or
land), title cards and the filmed inserts; `plan` gives each race shot a circuit and a camera
(`Tools/film/`, kept: timings only); the game films a circuit's shots with
`-BRRFilm=Tools/film/intro_plan.json -BRRFilmOut=<dir> -BRRAutopilot` on its map (`ABRRFilm`: a fixed
30 frames a second, the ghosts racing as recorded); `-run=BRRUpscaleFrames` redraws the title cards
as flat art; `assemble` cuts it all together over the original's sound into
`Saved/Remaster/Intro.mp4` (ffmpeg from `pip install imageio-ffmpeg`), which the remastered look
then plays instead of the original. The result holds the original's sound and stays out of git.

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
