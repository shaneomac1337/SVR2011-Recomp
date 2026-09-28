# Start a player-driven session that records every frame and every event that can delay one
# (pipeline builds, presents skipped while they build, blocking readbacks). Uses the launcher's
# saved settings. While playing, run scripts/mark.ps1 just before the moment to inspect, then
# close the game and run scripts/analyze_hitches.py on the folder printed below.
param([string]$SettingsPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$SettingsPath) { $SettingsPath = "$projectRoot/userdata/launcher/settings.json" }
$run = Join-Path $projectRoot ("analysis/hitch-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
& "$PSScriptRoot/play-vulkan.ps1" -PerfCapture -SettingsPath $SettingsPath -RunDirectory $run `
    -ExtraArguments '--frame_event_log=true'
