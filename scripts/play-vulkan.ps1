# Launch the separate Vulkan-only build without a console window or test timeout.
param(
    [switch]$PerfCapture,
    [switch]$Profile,
    [switch]$SynchronousShaders,
    [switch]$KeyboardInput,
    [switch]$SkipPlaceholderPipelines,
    # FSI keeps EDRAM contents across frames the guest does not redraw; see docs/performance.md.
    [ValidateSet('fbo', 'fsi')][string]$RenderTargetPath = 'fsi',
    [string]$SettingsPath,
    [string]$RunDirectory,
    [ValidateSet('Baseline', 'InvalidFetch')][string]$Experiment = 'InvalidFetch',
    # Extra runtime cvars for experiments, for example '--gpu_wait_reg_mem_stats=true'.
    [string[]]$ExtraArguments = @()
)
$ErrorActionPreference = 'Stop'
$sessionMutex = [Threading.Mutex]::new($false, 'Local\SVR2011-Vulkan-Game')
$ownsSession = $false
try { $ownsSession = $sessionMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsSession = $true }
if (!$ownsSession) { $sessionMutex.Dispose(); throw 'A Vulkan game session is already running.' }
try {
if (Get-Process svr2011 -ErrorAction SilentlyContinue) { throw 'The game is already running. Close it normally before starting another session.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot/Launcher.Core.ps1"
$settings = $null
if ($SettingsPath) { $settings = Read-SvrSettings $SettingsPath }
$executable = "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan/svr2011.exe"
if (!(Test-Path $executable)) { throw 'Build first with scripts/build.ps1 -Renderer Vulkan.' }
$run = if ($RunDirectory) { $RunDirectory } else {
    Join-Path $projectRoot ("analysis/vulkan-play-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
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
if ($settings) {
    foreach ($argument in (Get-SvrDisplayArguments $settings)) { $info.ArgumentList.Add($argument) }
    if ($settings.perfCapture) { $PerfCapture = $true }
}
if ($PerfCapture) { $info.ArgumentList.Add("--perf_log_csv=$run/perf.csv") }
if ($Profile) { $info.ArgumentList.Add('--svr_profile=true') }
if ($SynchronousShaders) { $info.ArgumentList.Add('--async_shader_compilation=false') }
# Keyboard-to-controller emulation lets automated loops drive menus.
if ($KeyboardInput) { $info.ArgumentList.Add('--mnk_mode=true') }
# Experimental: defer new pipelines without a synchronous placeholder compile on the GPU thread.
if ($SkipPlaceholderPipelines) { $info.ArgumentList.Add('--vulkan_async_skip_placeholder_pipelines=true') }
$info.ArgumentList.Add("--render_target_path_vulkan=$RenderTargetPath")
if ($Experiment -eq 'InvalidFetch') {
    $info.ArgumentList.Add('--gpu_allow_invalid_fetch_constants=true')
}
foreach ($argument in $ExtraArguments) {
    if ($argument -notmatch '^--[a-z0-9_]+=') { throw "Invalid runtime argument: $argument" }
    $info.ArgumentList.Add($argument)
}
$runtimeHashes = [ordered]@{}
foreach ($name in @('svr2011.exe','rexruntimerd.dll','rexgpu-xenosrd.dll','TracyClientrd.dll')) {
    $runtimeHashes[$name] = (Get-FileHash -LiteralPath (Join-Path (Split-Path $executable -Parent) $name) -Algorithm SHA256).Hash
}
[ordered]@{
    executable = $executable; experiment = $Experiment; perf_capture = [bool]$PerfCapture; profile = [bool]$Profile
    settings = $settings
    synchronous_shaders = [bool]$SynchronousShaders
    keyboard_input = [bool]$KeyboardInput
    skip_placeholder_pipelines = [bool]$SkipPlaceholderPipelines
    render_target_path = $RenderTargetPath
    runtime_sha256 = $runtimeHashes
    arguments = @($info.ArgumentList)
} | ConvertTo-Json -Depth 4 | Set-Content "$run/launch.json"
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
        executable = $executable; process_id = $process.Id; renderer = 'Vulkan'; experiment = $Experiment
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
} finally {
    $sessionMutex.ReleaseMutex()
    $sessionMutex.Dispose()
}
