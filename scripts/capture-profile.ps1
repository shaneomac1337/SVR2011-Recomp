param([ValidateRange(1, 300)][int]$Seconds = 30)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
$capture = "$projectRoot/.tools/tracy-0.13.1/tracy-capture.exe"
if (!(Test-Path $capture)) { throw 'Run scripts/prepare-profiler.ps1 first.' }
$games = @(Get-Process svr2011 -ErrorAction SilentlyContinue)
if ($games.Count -ne 1) { throw 'Start exactly one game before capturing its profile.' }
$listener = Get-NetTCPConnection -State Listen -LocalPort 8086 -ErrorAction SilentlyContinue |
    Where-Object OwningProcess -eq $games[0].Id
if (!$listener) { throw 'This game has no Tracy listener. Start the next session with play-vulkan.ps1 -Profile.' }
$run = "$projectRoot/analysis/profile-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
New-Item -ItemType Directory -Path $run | Out-Null
Write-Output "Capturing $Seconds seconds to $run. Only the profiler capture ends; the game is not stopped."
Invoke-Hidden $capture @('-a', '127.0.0.1', '-o', "$run/session.tracy", '-s', "$Seconds") -LogPath "$run/capture.log"
Invoke-Hidden "$projectRoot/.tools/tracy-0.13.1/tracy-csvexport.exe" @('-e', "$run/session.tracy") -LogPath "$run/zones-self.csv"
Write-Output "Profile saved in $run"
