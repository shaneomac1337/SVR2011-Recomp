# Launch the separate Vulkan-only build without a console window or test timeout.
param([switch]$PerfCapture)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$executable = "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan/svr2011.exe"
if (!(Test-Path $executable)) { throw 'Build first with scripts/build.ps1 -Renderer Vulkan.' }
$run = Join-Path $projectRoot ("analysis/vulkan-play-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $executable
$info.WorkingDirectory = Split-Path $executable -Parent
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
foreach ($argument in @('--gpu_plugin=xenos', '--vulkan_device=-1',
    "--game_data_root=$projectRoot/assets", "--user_data_root=$projectRoot/userdata/vulkan",
    "--cache_root=$projectRoot/cache/vulkan", "--log_file=$run/runtime.log",
    '--log_level=info', '--log_flush_interval=1')) {
    $info.ArgumentList.Add($argument)
}
if ($PerfCapture) { $info.ArgumentList.Add("--perf_log_csv=$run/perf.csv") }
$process = [System.Diagnostics.Process]::Start($info)
Write-Output "Vulkan game started (PID $($process.Id)). Logs: $run"
Write-Output 'No timeout is set. Close the game window when you are finished.'
$timer = [System.Diagnostics.Stopwatch]::StartNew()
try {
    # Observe the exit without ever terminating the user's interactive session.
    $process.WaitForExit()
    $fatalTargets = @()
    if (Test-Path "$run/runtime.log") {
        $fatalTargets = @(Select-String -Path "$run/runtime.log" -Pattern '\[FATAL\].*guest address (0x[0-9A-Fa-f]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
    }
    $outcome = if ($process.ExitCode -eq 0) { 'exited' } else { 'crashed' }
    [ordered]@{
        executable = $executable; process_id = $process.Id; renderer = 'Vulkan'
        elapsed_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 2)
        exit_code = $process.ExitCode; outcome = $outcome
        fatal_guest_targets = $fatalTargets; logs = $run
    } | ConvertTo-Json | Set-Content "$run/result.json"
    if ($outcome -eq 'crashed') {
        throw "Game process crashed (exit code $($process.ExitCode), missing targets: $($fatalTargets -join ', ')). Logs: $run"
    }
    Write-Output "Game exited normally. Logs: $run"
} finally {
    $process.Dispose()
}
