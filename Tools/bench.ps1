# Repeatable performance bench: autopilot lap(s) with pinned weather, perf log, CSV profile and a
# labelled screenshot, collected under Tools/bench/results/<Label>_<timestamp>/.
#
#   Tools\bench.ps1 -Level DIRT1 -Weather 2 -Seed 1234 -Label crowd-before
#
# The screenshot gets the label and the numbers stamped into the image (bench_report.py), so a
# folder of shots stays self-describing. Compare runs by label; two runs of the same label should
# agree within ~2-3%.
param(
    [string]$Level = "DIRT1",
    [string]$Vehicle = "JEEP",
    [int]$Look = 1,            # 0 classic, 1 remastered
    [int]$Weather = 2,         # 0 clear, 2 foul (pinned by -Seed either way)
    [int]$Seed = 1234,
    [int]$Seconds = 90,
    [string]$Label = "bench",
    [string]$Editor = "D:\Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe",
    [switch]$Insights,
    [switch]$OffScreen
)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Project = Join-Path $Root "Game\BiggerRedRacing.uproject"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$Out = Join-Path $PSScriptRoot "bench\results\${Label}_$Stamp"
New-Item -ItemType Directory -Force $Out | Out-Null
$LogFile = Join-Path $Out "run.log"
$ShotName = "$Label.png"
$Started = Get-Date

$Args = @(
    "`"$Project`"",
    "/Game/BRR/$Level/L_${Level}?Vehicle=$Vehicle",   # ${}: a bare $Level? would parse as the variable "Level?..."
    "-game", "-windowed", "-ResX=1920", "-ResY=1080", "-nosplash",
    "-BRRAutopilot", "-BRRWeather=$Weather", "-BRRWeatherSeed=$Seed",
    "-BRRShot=$Seconds", "-BRRShotName=$ShotName",
    "-ExecCmds=`"brr.Look $Look, brr.LogPerf 1, t.IdleWhenNotForeground 0`"",
    "-csvCaptureFrames=$($Seconds * 60)",   # must finish before the quit to be written, so assumes >=60 fps average
    "-abslog=`"$LogFile`""
)
if ($OffScreen) { $Args += "-RenderOffScreen" }
if ($Insights) { $Args += @("-trace=default,gpu", "-tracefile=`"$(Join-Path $Out "$Label.utrace")`"") }

Write-Host "bench: $Label ($Level $Vehicle look=$Look weather=$Weather seed=$Seed, $Seconds s)"
$P = Start-Process -FilePath $Editor -ArgumentList $Args -Wait -PassThru
if ($P.ExitCode -ne 0) { Write-Warning "editor exited with $($P.ExitCode)" }

# Collect the screenshot and the CSV this run produced; the log names their real paths (an
# installed-engine run saves under AppData's UnrealEngine/<ver>/Saved, not the project).
$LogText = Get-Content $LogFile -Raw
if ($LogText -match 'Writing CSV to file : (\S+\.csv)' -and (Test-Path $Matches[1])) {
    Move-Item $Matches[1] (Join-Path $Out "run.csv") -Force
}
if ($LogText -match 'BRRShot: requested (\S+\.png)' -and (Test-Path $Matches[1])) {
    Move-Item $Matches[1] (Join-Path $Out $ShotName) -Force
}

$ReportArgs = @("$PSScriptRoot\bench_report.py", "--log", $LogFile,
    "--label", "$Label | $Level $Vehicle look=$Look weather=$Weather seed=$Seed")
if (Test-Path (Join-Path $Out "run.csv")) { $ReportArgs += @("--csv", (Join-Path $Out "run.csv")) }
if (Test-Path (Join-Path $Out $ShotName)) { $ReportArgs += @("--image", (Join-Path $Out $ShotName)) }
$Summary = python @ReportArgs
$Summary | Tee-Object -FilePath (Join-Path $Out "summary.txt")
