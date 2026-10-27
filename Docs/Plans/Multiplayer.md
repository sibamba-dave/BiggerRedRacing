# Plan (on hold): internet multiplayer — lockstep, direct IP

Drafted 2026-10-27 and set aside unstarted; kept here so it survives. Decisions taken with the user:
direct IP (UDP 47315); 2–4 humans + .REC ghosts filling the grid; working prototype first; lockstep —
online every human car runs on the original physics (`VehicleSim::step`) as boats/helis already do,
Chaos stays single-player. Work on a branch `multiplayer`.

## Context
The original game had network, modem and split-screen multiplayer. BRR has its screens hidden: the
CONFIG_2P box (`BRRMenuScreens.cpp:182-190`), the globe screen's multiplayer picks (`:235`) and the
results screen's network wait (`:461`). The game has no networking today.

The vendored OpenRedRacing (ORR) sim is integer-only and deterministic, and already carries a
lockstep core: `RaceSession`, `LockstepInputs`, `snapshot` and `checksum`. Upstream ORR also has a
tested lockstep network stack over ENet (`src/net/*`, `src/game/net_race_driver.*`) and a harness
(`net_bench`), none of it vendored yet.

## Steps (each its own commit)
1. **Vendoring** (`Tools/sync_orr.py`): add `net/{net_protocol,net_game,discovery}` → `ORR/net/`,
   `game/net_race_driver` → `ORR/game/`, `scene/race_level` (provides
   `build_race_level`/`build_race_field`). ENet 1.3.18 as a `ThirdParty/enet` submodule; the script
   copies `.c` + headers into `BRRFormats/Private/ThirdParty/enet/` with LICENSE. Generate
   `ORR/orr_version.h` (commit SHA).
2. **Build** (`BRRFormats.Build.cs`): enet include path, `ws2_32`/`winmm`,
   `_WINSOCK_DEPRECATED_NO_WARNINGS`. Only ENet's `.c` and `net_game.cpp`/`discovery.cpp` include
   `enet.h` (no PCH/unity keeps winsock away from UE headers). Fallback: External module with a
   prebuilt `.lib`. Smoke test host+join loopback.
3. **Behaviour-neutral `FBRRRace::FImpl` refactor**: car/race/terrain accessors; split
   `AdvanceField` into `AdvanceRules` (offline only) and `AdvancePresentation` (shared);
   `BRRRaceSound::OnTick` takes `const VehicleSim*, Count`. Prove with `BRR.Formats.Race.*` +
   `-BRRShot`.
4. **Net wrapper** (new `BRRFormats/Public/BRRNet.h`, `Private/BRRNet.cpp`, `Private/BRRNetImpl.h`):
   `FBRRNetGame` wraps `bigred::NetGame` (Host/Join/Leave/Service; lobby players/ready/vehicle/
   phase/level; host SetLevel (ghosts on, max 4, 1 race)/BeginVehicleChoice/BackToLobby; pending
   start/RaceId; SetInputLoss). **Content guard** in `NetLook.custom_logo` (relayed opaquely): BRR
   net version, ORR commit hash, FNV of `.LEV` + `.VHC` bytes, imported flag; the host refuses
   mismatches ("DATA MISMATCH"/"NOT IMPORTED").
5. **Online `FBRRRace`**: `CreateOnline(DataDir, FBRRNetGame&)` — `build_race_level` +
   `build_race_field` from the host's START (humans blocks 0..n-1, ghosts rest),
   `Session->rules = start.rules`, configure as `race_screen.cpp:192-348`, `Driver.begin`.
   `NetFrame(Accumulator, dt, LocalInput, OnTick, OnResynced)` + `NetFrameDrawn()`. `NetStatus()`
   (waiting-for, stall ms, resync banner, desync tick, error), `Checksum()`, `Ticks()`, `IsOnline`,
   `IsHuman`, `NameOf`, `IsNetRaceOver()`. `RacerTransform` uses `Driver.drawn_pose` (resyncs
   slide). Online no-ops: `Tick`, `TickSimulated`, `RecoverPlayerAt`, `KnockOver`,
   `ReportPlayerImpact`; `ABRRWrecks` must not feed Chaos hits into the sim. Log
   `tick N checksum X` every 250 ticks (`LogBRRNet`).
6. **Game instance**: owns `TUniquePtr<FBRRNetGame>` serviced by `FTSTicker` (survives menus + map
   loads). Pending START → `OpenLevel(L_<level>, "Online")`; errors → front end. Dev flags
   `-BRRHost=N -BRRJoin=ip[:port] -BRRNetName= -BRRNetLevel= -BRRNetVehicle= -BRRNetAutodrive
   -BRRNetQuitAfter=T -BRRNetLoss=N -BRRNetDesyncAt=T`; console
   `brr.NetHost/NetJoin/NetLeave/NetStatus`.
7. **Race game mode**: `?Online` → `CreateOnline`; local human = `ABRRVehiclePawn` with a new
   `bForceSimulated` flag (`BRRVehiclePawn.cpp:220`); remote humans + ghosts = `ABRRGhostCar`;
   weather seeded from `RaceId`. Online Tick branch replaces `BRRRaceGameMode.cpp:392-449`: sample
   `TakeSimulatedInput`/autodrive, call `NetFrame` (no 10-tick clamp). Skip Chaos
   bounce/impact/recover online; the autopilot presses the recover bit. Race end counted in ticks so
   machines leave together; results for N humans; host `BackToLobby`; Escape leaves the net game.
   HUD: "WAITING FOR <name>" (>300 ms), "RESYNCHRONISED", errors.
8. **Front end**: unhide CONFIG_2P → new `FNetScreen` on the NETSCN backdrop, text via
   `FBRRMenuPainter`; add `FBRRMenuScreen::OnKey` for the IP field. Pre-game: HOST / JOIN <ip> /
   NAME / BACK (last IP in `[BRR.Net]`). In-game: player rows (name, ready, vehicle, content OK),
   host cycles circuit, READY, host START → vehicle choice, host IP:port shown. `ReturnToFrontEnd`
   reopens the lobby while active.

## Reuse
ORR: `RaceSession`, `LockstepInputs`, `snapshot`, `RaceRules::network()`, `NetGame`,
`NetRaceDriver`, `build_race_field`. References:
`ThirdParty/OpenRedRacing/src/game/race_screen.cpp` (:192-348, :720-830), `src/net_bench.cpp`.
BRR: `ABRRGhostCar`, `TakeSimulatedInput`/`SetSimulatedPose`, `FBRRMenuScreen`/`FBRRMenuPainter`,
`UBRRGameInstance::IsImported`.

## Verification
- New `BRRFormats/Private/Tests/BRRNetTest.cpp`: `Net.Wire` (port of net_bench test_wire);
  `Net.Loopback` (3 NetGames on loopback, lobby→START→scripted race vs reference run; +20% input
  loss; +forced nudge → resync; +client leaving); `Net.RaceLockstep` (two `CreateOnline` via
  NetFrame, 1500 ticks, equal checksums, DIRT1 + WATER1); `Net.Snapshot` (save/load/run on,
  checksums match).
- `BRR.Formats.Race.*` still pass after step 3.
- New `Tools/net_loopback.ps1`: host `-BRRHost=2 -BRRNetLevel=DIRT1 -BRRNetAutodrive
  -BRRNetQuitAfter=3000 -abslog=host.log` + client `-BRRJoin=127.0.0.1 ... -abslog=client.log`;
  diff checksum lines, desync 0; variants `-BRRNetLoss=20`, `-BRRNetDesyncAt=500` (one resync).
  Keep `t.IdleWhenNotForeground 0`.
- Latency: clumsy on loopback, 50–150 ms + jitter + loss (input delay 3 ticks = 84 ms).
- Real internet: two PCs over Tailscale or forwarded UDP 47315.

## Known prototype limits
Online wheeled cars handle like the original (not the Chaos tuning) and wheels don't spin yet;
single race only (no series/league); no NAT punch-through or server browser.
