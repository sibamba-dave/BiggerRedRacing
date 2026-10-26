# Vehicle handling: Chaos fitted to the original

The player's car is a Chaos wheeled vehicle (`ABRRVehiclePawn`). Its targets come from the original
vehicle simulation itself: `BRR::MeasureHandling` runs ORR's port of the original physics on a
synthetic flat map, once per surface type, and `-BRRDriveTest` runs the same tests on the Chaos car.

## What Chaos does and what the original's model does

| Part | Owner |
|---|---|
| Engine, gearing, brakes, reverse | Chaos, fitted: one forward gear so the rev limiter is the original's top speed; reverse ratio from the original's reverse speed; torque from its 0-60% time with a falling torque curve; brake torque from its stopping time |
| Suspension, gravity, contacts, collisions | Chaos |
| Steering and sideways grip | The original's model, applied to the Chaos body (`ApplyOriginalSteering`): heading turns at the original's full-lock yaw rate for the current speed (separate curves under power and coasting, 0.64 s build-up), sideways slip decays as `svel` does, full-lock turns scrub speed towards the original's steady turning speed. Chaos tyres have low cornering stiffness so they do not fight it. |

The original corners at ~6 g (78°/s at 42 m/s for the JEEP): an arcade model no tyre reproduces,
hence the split.

## JEEP on TRACK (drive test, 2026-10-26)

| | original | Chaos | |
|---|---|---|---|
| top speed | 60.67 m/s | 60.67 m/s | +0.0% |
| 0-60% | 1.71 s | 1.76 s | +3.1% |
| 0-90% | 3.42 s | 3.56 s | +4.1% |
| top speed to stop | 1.82 s | 1.71 s | -6.3% |
| reverse top speed | 26.10 m/s | 26.17 m/s | +0.3% |
| full-lock yaw rate | 78.2 °/s | 78.4 °/s | +0.3% |
| full-lock speed | 42.4 m/s | 44.1 m/s | +3.9% |

Re-run: `UnrealEditor.exe Game\BiggerRedRacing.uproject /Game/BRR/DIRT1/L_DIRT1 -game -RenderOffScreen -BRRDriveTest[=VEHICLE]`
(from PowerShell); the table is in the log under `LogBRRDriveTest`.

## Not yet fitted
- Surfaces other than TRACK (the terrain has no per-surface physical materials yet; the original's
  SLIPPY / GRASS / SAND numbers are measured, see `BRR.Formats.Race.JeepHandling`).
- Airborne behaviour (the original's air pitch/roll rates and righting), bounce, the tilter, nitro,
  damage.
- Vehicles other than the JEEP have not been drive-tested; they use the same fit.
- Helicopters and hovercraft need their own movement.
