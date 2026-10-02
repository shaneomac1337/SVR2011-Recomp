# Shared, UI-independent settings and argument validation.
function New-SvrSettings {
    [ordered]@{ version = 1; displayMode = 'Borderless'; windowSize = '1280x720'; scale = 1; controller = 'Auto'; perfCapture = $false; presentation = 'Fifo'; framePacing = 'Auto'; sharpening = $true; renderer = 'Classic' }
}

function Test-SvrSettings($Settings) {
    if ($null -eq $Settings -or $Settings.version -ne 1) { throw 'Unsupported launcher settings version.' }
    if ($Settings.displayMode -notin @('Borderless', 'Windowed')) { throw 'Invalid display mode.' }
    if ($Settings.windowSize -notin @('1280x720', '1600x900', '1920x1080')) { throw 'Invalid window size.' }
    if ($Settings.scale -isnot [int] -and $Settings.scale -isnot [long]) { throw 'Invalid resolution scale.' }
    if ($Settings.scale -notin @(1, 2, 3)) { throw 'Invalid resolution scale.' }
    if ($Settings.controller -notin @('Auto', 'Xbox')) { throw 'Invalid controller mode.' }
    if ($Settings.perfCapture -isnot [bool]) { throw 'Invalid performance capture setting.' }
    if ($Settings.presentation -notin @('Immediate', 'Mailbox', 'Fifo')) { throw 'Invalid display synchronization mode.' }
    if ($Settings.framePacing -notin @('Auto', 'Game', 'Even')) { throw 'Invalid frame pacing mode.' }
    if ($Settings.sharpening -isnot [bool]) { throw 'Invalid sharpening setting.' }
    if ($Settings.renderer -notin @('Classic', 'Native')) { throw 'Invalid renderer.' }
}

function Read-SvrSettings([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return New-SvrSettings }
    $settings = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    # Older version-one files predate display synchronization controls.
    if (!$settings.Contains('presentation')) { $settings.presentation = 'Fifo' }
    if (!$settings.Contains('framePacing')) { $settings.framePacing = 'Auto' }
    # 'Even' was a separate choice until Automatic became even pacing on every monitor.
    if ($settings.framePacing -eq 'Even') { $settings.framePacing = 'Auto' }
    if (!$settings.Contains('sharpening')) { $settings.sharpening = $true }
    if (!$settings.Contains('renderer')) { $settings.renderer = 'Classic' }
    Test-SvrSettings $settings
    return $settings
}

function Save-SvrSettings([string]$Path, $Settings) {
    Test-SvrSettings $Settings
    $directory = Split-Path $Path -Parent
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
    $temp = Join-Path $directory ([System.IO.Path]::GetRandomFileName())
    try {
        [System.IO.File]::WriteAllText($temp, ($Settings | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::Move($temp, $Path, $true)
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}

# ScreenHeight: the screen's height in pixels for borderless fullscreen (0 = unknown).
function Get-SvrDisplayArguments($Settings, [int]$ScreenHeight = 0) {
    Test-SvrSettings $Settings
    $size = $Settings.windowSize.Split('x')
    '--fullscreen=' + ($Settings.displayMode -eq 'Borderless').ToString().ToLowerInvariant()
    "--window_width=$($size[0])"
    "--window_height=$($size[1])"
    "--draw_resolution_scale_x=$($Settings.scale)"
    "--draw_resolution_scale_y=$($Settings.scale)"
    # Specify the alias too so a previously persisted runtime value cannot override it.
    "--resolution_scale=$($Settings.scale)"
    '--input_backend=' + $(if ($Settings.controller -eq 'Xbox') { 'xinput' } else { 'sdl' })
    '--vulkan_allow_present_mode_immediate=' + ($Settings.presentation -eq 'Immediate').ToString().ToLowerInvariant()
    '--vulkan_allow_present_mode_mailbox=' + ($Settings.presentation -ne 'Fifo').ToString().ToLowerInvariant()
    '--vulkan_allow_present_mode_fifo_relaxed=' + ($Settings.presentation -eq 'Immediate').ToString().ToLowerInvariant()
    '--present_pace_to_guest_vblank=' + (Test-SvrPacesToGuestVblank $Settings).ToString().ToLowerInvariant()
    # FSR 1 upscales when the game image is smaller than the screen; otherwise CAS sharpens or downsamples it.
    '--present_effect=' + $(if ($Settings.sharpening) { 'fsr' } else { 'bilinear' })
    '--present_fsr_sharpness_reduction=0.5'
    if ($Settings.renderer -eq 'Native') {
        '--svr_native_renderer=true'
        "--svr_native_resolution_scale=$($Settings.scale)"
        '--svr_native_lod_bias=' + (Get-SvrNativeLodBias $Settings $ScreenHeight)
    }
}

# Sharper distant textures cost shimmer, unless each screen pixel averages several rendered ones.
function Get-SvrNativeLodBias($Settings, [int]$ScreenHeight) {
    $outputHeight = if ($Settings.displayMode -eq 'Windowed') { [int]$Settings.windowSize.Split('x')[1] } else { $ScreenHeight }
    if ($outputHeight -gt 0 -and 720 * $Settings.scale -gt $outputHeight) { return '-1' }
    return '-0.5'
}

# The primary screen's height in physical pixels, whatever the DPI scaling.
function Get-SvrScreenHeight {
    if (!('SvrDisplay' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class SvrDisplay {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DevMode {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
        public short SpecVersion, DriverVersion, Size, DriverExtra;
        public int Fields, PositionX, PositionY, Orientation, FixedOutput;
        public short Color, Duplex, YResolution, TTOption, Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
        public short LogPixels;
        public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);
    public static int Height() {
        var mode = new DevMode();
        mode.Size = (short)Marshal.SizeOf(typeof(DevMode));
        return EnumDisplaySettings(null, -1, ref mode) ? mode.PelsHeight : 0;
    }
}
'@
    }
    return [SvrDisplay]::Height()
}

# The game advances a fixed step per frame, so each frame belongs on an even 60 Hz beat. The
# runtime shows it 5 ms after its vblank, just after the game finishes it, on every display.
function Test-SvrPacesToGuestVblank($Settings) {
    return $Settings.framePacing -ne 'Game'
}
