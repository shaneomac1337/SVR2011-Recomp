# Launch the separate Vulkan-only build without a console window or test timeout.
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
$process = [System.Diagnostics.Process]::Start($info)
Write-Output "Vulkan game started (PID $($process.Id)). Logs: $run"
$process.Dispose()
