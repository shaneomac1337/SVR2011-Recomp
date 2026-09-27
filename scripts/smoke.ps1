param(
    [ValidateRange(1, 300)][int]$Seconds = 20,
    [ValidateSet('info', 'debug', 'trace')][string]$LogLevel = 'info',
    [ValidateSet('D3D12', 'Vulkan')][string]$Renderer = 'D3D12',
    [ValidateSet('Warp', 'Hardware')][string]$Adapter = 'Warp',
    [switch]$GpuDiagnostics,
    [switch]$PerfCapture
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildName = 'win-amd64-relwithdebinfo'
$cacheRoot = "$projectRoot/cache"
$userRoot = "$projectRoot/userdata"
if ($Renderer -eq 'Vulkan') {
    if ($PSBoundParameters.ContainsKey('Adapter') -and $Adapter -eq 'Warp') {
        throw 'WARP is a D3D12 adapter. Vulkan uses the hardware Vulkan driver.'
    }
    $Adapter = 'Hardware'
    $buildName += '-vulkan'
    $cacheRoot += '/vulkan'
    $userRoot += '/vulkan'
}
$executable = "$projectRoot/out/build/$buildName/svr2011.exe"
if (!(Test-Path $executable)) { throw "Build $Renderer first: $executable" }
$run = Join-Path $projectRoot ("analysis/runtime-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $executable
$info.WorkingDirectory = Split-Path $executable -Parent
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
foreach ($argument in @('--headless', '--gpu_plugin=xenos', "--game_data_root=$projectRoot/assets",
    "--user_data_root=$userRoot", "--cache_root=$cacheRoot",
    "--log_file=$run/runtime.log", "--log_level=$LogLevel", '--log_flush_interval=1')) {
    $info.ArgumentList.Add($argument)
}
if ($Renderer -eq 'Vulkan') {
    $info.ArgumentList.Add('--vulkan_device=-1')
    if ($GpuDiagnostics) { $info.ArgumentList.Add('--vulkan_validation_enabled=true') }
} else {
    $info.ArgumentList.Add($(if ($Adapter -eq 'Warp') { '--d3d12_adapter=-2' } else { '--d3d12_adapter=-1' }))
    if ($GpuDiagnostics) { $info.ArgumentList.Add('--d3d12_debug') }
}
$process = [System.Diagnostics.Process]::new()
if ($PerfCapture) { $info.ArgumentList.Add("--perf_log_csv=$run/perf.csv") }
$process.StartInfo = $info
$started = $false
$timer = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $started = $process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = !$process.WaitForExit($Seconds * 1000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $stdout.GetAwaiter().GetResult() | Set-Content "$run/stdout.txt"
    $stderr.GetAwaiter().GetResult() | Set-Content "$run/stderr.txt"
    $fatalTargets = @()
    if (Test-Path "$run/runtime.log") {
        $fatalTargets = @(Select-String -Path "$run/runtime.log" -Pattern '\[FATAL\].*guest address (0x[0-9A-Fa-f]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
    }
    $outcome = if ($timedOut) { 'alive_at_deadline' } elseif ($process.ExitCode -eq 0) { 'exited' } else { 'crashed' }
    $result = [ordered]@{
        executable = $executable; seconds = $Seconds; elapsed_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 2)
        timed_out = $timedOut; exit_code = $process.ExitCode; outcome = $outcome
        fatal_guest_targets = $fatalTargets; adapter = $Adapter; renderer = $Renderer; logs = $run
    }
    $result | ConvertTo-Json | Set-Content "$run/result.json"
    $result | ConvertTo-Json
    if ($outcome -eq 'crashed') { throw "Game crashed. Diagnostics saved in $run" }
} finally {
    if ($started -and !$process.HasExited) { $process.Kill($true); $process.WaitForExit() }
    $process.Dispose()
}
