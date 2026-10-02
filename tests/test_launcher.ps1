$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Launcher.Core.ps1"
function Assert($Value, [string]$Message) { if (!$Value) { throw $Message } }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$path = Join-Path $scratch 'settings.json'
try {
    $defaults = Read-SvrSettings $path
    Assert ($defaults.scale -eq 1 -and !$defaults.perfCapture) 'Safe defaults failed.'
    $defaults.displayMode = 'Windowed'
    $defaults.windowSize = '1920x1080'
    $defaults.scale = 3
    $defaults.controller = 'Xbox'
    $defaults.perfCapture = $true
    Save-SvrSettings $path $defaults
    $loaded = Read-SvrSettings $path
    Assert ($loaded.scale -eq 3 -and $loaded.perfCapture) 'Settings did not round-trip.'
    $arguments = @(Get-SvrDisplayArguments $loaded)
    foreach ($argument in @('--fullscreen=false','--window_width=1920','--window_height=1080',
        '--draw_resolution_scale_x=3','--draw_resolution_scale_y=3','--resolution_scale=3','--input_backend=xinput')) {
        Assert ($arguments -contains $argument) "Missing argument: $argument"
    }
    foreach ($mode in @('Immediate','Mailbox','Fifo')) {
        $value = New-SvrSettings
        $value.presentation = $mode
        $argsForMode = @(Get-SvrDisplayArguments $value)
        Assert ($argsForMode -notcontains '--vsync=false') 'Presentation setting changed guest clock.'
        $immediate = ($mode -eq 'Immediate').ToString().ToLowerInvariant()
        Assert ($argsForMode -contains "--vulkan_allow_present_mode_immediate=$immediate") 'Wrong immediate mode.'
        if ($mode -eq 'Fifo') {
            Assert ($argsForMode -contains '--vulkan_allow_present_mode_mailbox=false') 'FIFO did not disable mailbox.'
            Assert ($argsForMode -contains '--vulkan_allow_present_mode_fifo_relaxed=false') 'FIFO did not disable relaxed FIFO.'
        }
    }
    $sharp = New-SvrSettings
    Assert ((Get-SvrDisplayArguments $sharp) -contains '--present_effect=fsr') 'Sharpening on did not select FSR.'
    $sharp.sharpening = $false
    Assert ((Get-SvrDisplayArguments $sharp) -contains '--present_effect=bilinear') 'Sharpening off did not select bilinear.'
    $native = New-SvrSettings
    Assert (!(@(Get-SvrDisplayArguments $native 1440) -like '--svr_native*')) 'Classic renderer passed native arguments.'
    $native.renderer = 'Native'
    $native.scale = 2
    $nativeArgs = @(Get-SvrDisplayArguments $native 1440)
    foreach ($argument in @('--svr_native_renderer=true', '--svr_native_resolution_scale=2', '--svr_native_lod_bias=-0.5')) {
        Assert ($nativeArgs -contains $argument) "Missing native argument: $argument"
    }
    # Rendering above the output resolution supersamples, so the sharper bias does not shimmer.
    foreach ($case in @(@('Borderless', '1280x720', 3, 1440, '-1'), @('Borderless', '1280x720', 2, 2160, '-0.5'),
        @('Borderless', '1280x720', 3, 0, '-0.5'), @('Windowed', '1600x900', 2, 2160, '-1'), @('Windowed', '1920x1080', 1, 720, '-0.5'))) {
        $native.displayMode = $case[0]; $native.windowSize = $case[1]; $native.scale = $case[2]
        Assert ((Get-SvrDisplayArguments $native $case[3]) -contains "--svr_native_lod_bias=$($case[4])") "Wrong bias for $($case -join ' ')."
    }
    foreach ($case in @(@('Auto', 'true'), @('Game', 'false'))) {
        $value = New-SvrSettings
        $value.framePacing = $case[0]
        Assert ((Get-SvrDisplayArguments $value) -contains "--present_pace_to_guest_vblank=$($case[1])") "Wrong pacing for $($case[0])."
    }
    $legacy = Join-Path ([IO.Path]::GetTempPath()) "svr-even-$PID.json"
    try {
        '{"version":1,"displayMode":"Windowed","windowSize":"1280x720","scale":1,"controller":"Auto","perfCapture":false,"presentation":"Fifo","framePacing":"Even"}' |
            Set-Content -LiteralPath $legacy
        Assert ((Read-SvrSettings $legacy).framePacing -eq 'Auto') 'Saved Even pacing did not load as Automatic.'
        Assert ((Read-SvrSettings $legacy).sharpening -eq $true) 'Settings without sharpening did not default to on.'
        Assert ((Read-SvrSettings $legacy).renderer -eq 'Classic') 'Settings without a renderer did not default to Classic.'
    } finally { Remove-Item -LiteralPath $legacy -ErrorAction SilentlyContinue }
    $bad = New-SvrSettings
    $bad.framePacing = 'Fast'
    $threw = $false; try { Test-SvrSettings $bad } catch { $threw = $true }
    Assert $threw 'Invalid frame pacing was accepted.'
    $old = New-SvrSettings
    $old.Remove('presentation')
    $old | ConvertTo-Json | Set-Content -LiteralPath $path
    Assert ((Read-SvrSettings $path).presentation -eq 'Fifo') 'Old settings did not migrate to the default.'
    Save-SvrSettings $path $loaded
    foreach ($field in @('scale','windowSize','controller','displayMode','perfCapture','version','presentation','renderer')) {
        $invalid = New-SvrSettings
        $invalid[$field] = 'invalid'
        $rejected = $false
        try { Save-SvrSettings $path $invalid } catch { $rejected = $true }
        Assert $rejected "Invalid $field was accepted."
        Assert ((Read-SvrSettings $path).scale -eq 3) 'Invalid save overwrote valid settings.'
    }
    Set-Content -LiteralPath $path -Value '{broken'
    $rejected = $false
    try { Read-SvrSettings $path } catch { $rejected = $true }
    Assert $rejected 'Corrupt settings silently accepted.'
    Write-Output 'Launcher settings, argument mapping and invalid-file checks passed.'
} finally {
    # Only remove the exact temporary file and then the now-empty test directory.
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    [IO.Directory]::Delete($scratch)
}
