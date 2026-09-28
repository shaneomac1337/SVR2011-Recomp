# Shared, UI-independent settings and argument validation.
function New-SvrSettings {
    [ordered]@{ version = 1; displayMode = 'Borderless'; windowSize = '1280x720'; scale = 1; controller = 'Auto'; perfCapture = $false; presentation = 'Fifo'; framePacing = 'Auto' }
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
}

function Read-SvrSettings([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return New-SvrSettings }
    $settings = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    # Older version-one files predate display synchronization controls.
    if (!$settings.Contains('presentation')) { $settings.presentation = 'Fifo' }
    if (!$settings.Contains('framePacing')) { $settings.framePacing = 'Auto' }
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

function Get-SvrDisplayArguments($Settings) {
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
}

# The game moves things by measured time, so frames look smoothest shown as soon as they are
# finished. Only a 60 Hz display without variable refresh needs them held to the tick.
function Test-SvrPacesToGuestVblank($Settings) {
    if ($Settings.framePacing -eq 'Auto') { return (Get-SvrPrimaryRefreshHz) -lt 100 }
    return $Settings.framePacing -eq 'Even'
}

function Get-SvrPrimaryRefreshHz {
    if (!('SvrDisplay' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SvrDisplay {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct DevMode {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    public short SpecVersion, DriverVersion, Size, DriverExtra;
    public int Fields, PositionX, PositionY, Orientation, FixedOutput;
    public short Color, Duplex, YResolution, TTOption, Collate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
    public short LogPixels;
    public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);
  public static int PrimaryRefreshHz() {
    var mode = new DevMode { Size = (short)Marshal.SizeOf(typeof(DevMode)) };
    return EnumDisplaySettings(null, -1, ref mode) ? mode.DisplayFrequency : 60;
  }
}
'@
    }
    [SvrDisplay]::PrimaryRefreshHz()
}
