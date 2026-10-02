param([switch]$SmokeTest, [string]$ScreenshotPath, [string]$SettingsPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
. "$PSScriptRoot/Launcher.Core.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$SettingsPath) { $SettingsPath = "$projectRoot/userdata/launcher/settings.json" }
$script:runner = $null
$script:runPath = $null
$script:busy = $false
$script:loading = $true
$script:warning = $null
try { $script:settings = Read-SvrSettings $SettingsPath } catch {
    $script:settings = New-SvrSettings
    $script:warning = 'Saved settings could not be read. Defaults loaded; use Save to replace them.'
}
$reader = [System.Xml.XmlNodeReader]::new([xml](Get-Content "$projectRoot/launcher/Launcher.xaml" -Raw))
try { $window = [Windows.Markup.XamlReader]::Load($reader) } finally { $reader.Close() }
$ui = @{}
foreach ($name in @('DisplayMode','WindowSize','Scale','ScaleHelp','Presentation','PresentationHelp','FramePacing','FramePacingHelp','Controller','PerfCapture','Sharpening','Renderer','RendererHelp','Save','Reset','Play','Logs','Status','SettingsPanel')) {
    $ui[$name] = $window.FindName($name)
}
function Set-Status([string]$Message, [bool]$ErrorState = $false) {
    $ui.Status.Text = $Message
    $ui.Status.Foreground = if ($ErrorState) { '#A32130' } else { '#414C65' }
}
function Select-Value($Control, $Value) {
    foreach ($item in $Control.Items) { if ([string]$item.Tag -eq [string]$Value) { $Control.SelectedItem = $item; break } }
}
function Update-DisplayHelp {
    $ui.WindowSize.IsEnabled = $ui.DisplayMode.SelectedItem.Tag -eq 'Windowed'
    $ui.ScaleHelp.Text = if ([int]$ui.Scale.SelectedItem.Tag -gt 1) {
        'More detail on models and the arena. Needs a stronger GPU: 3× uses about twice the GPU time of 1×.'
    } else { 'The original resolution is the validated setting. Window size does not change rendering detail.' }
    $ui.RendererHelp.Text = if ($ui.Renderer.SelectedItem.Tag -eq 'Native') {
        'Draws the game directly with Vulkan: sharper textures and lower GPU load. Still in testing; if something looks wrong, switch back to Classic.'
    } else { 'Draws the game the way the Xbox 360 does. Works on every supported GPU.' }
    $ui.PresentationHelp.Text = switch ($ui.Presentation.SelectedItem.Tag) {
        'Mailbox' { 'Syncs display output without a fixed FPS cap. Falls back to monitor VSync if unavailable. Needs a gameplay check.' }
        'Immediate' { 'Shows each frame as soon as it is ready. Slightly lower latency, but the image can tear.' }
        default { 'Each frame lands on a display refresh, without tearing. No FPS limiter needed.' }
    }
    $ui.FramePacingHelp.Text = if ($ui.FramePacing.SelectedItem.Tag -eq 'Game') {
        'Shows each frame the moment the game finishes it. Frames arrive 13-21 ms apart, so motion is less even.'
    } else { 'Shows every frame on an even 60 Hz beat, a few milliseconds after the game finishes it. Smoothest on every monitor.' }
}
function Show-Settings($Value) {
    $script:loading = $true
    Select-Value $ui.DisplayMode $Value.displayMode
    Select-Value $ui.WindowSize $Value.windowSize
    Select-Value $ui.Scale $Value.scale
    Select-Value $ui.Controller $Value.controller
    Select-Value $ui.Presentation $Value.presentation
    Select-Value $ui.FramePacing $Value.framePacing
    Select-Value $ui.Renderer $Value.renderer
    $ui.PerfCapture.IsChecked = $Value.perfCapture
    $ui.Sharpening.IsChecked = $Value.sharpening
    Update-DisplayHelp
    $script:loading = $false
}
function Read-Controls {
    $value = New-SvrSettings
    $value.displayMode = [string]$ui.DisplayMode.SelectedItem.Tag
    $value.windowSize = [string]$ui.WindowSize.SelectedItem.Tag
    $value.scale = [int]$ui.Scale.SelectedItem.Tag
    $value.controller = [string]$ui.Controller.SelectedItem.Tag
    $value.presentation = [string]$ui.Presentation.SelectedItem.Tag
    $value.framePacing = [string]$ui.FramePacing.SelectedItem.Tag
    $value.perfCapture = [bool]$ui.PerfCapture.IsChecked
    $value.sharpening = [bool]$ui.Sharpening.IsChecked
    $value.renderer = [string]$ui.Renderer.SelectedItem.Tag
    return $value
}
function Set-Dirty {
    if (!$script:loading) {
        Update-DisplayHelp
        Set-Status 'Unsaved changes. Play also saves your settings.'
    }
}
Show-Settings $script:settings
foreach ($name in @('DisplayMode','WindowSize','Scale','Controller','Presentation','FramePacing','Renderer')) { $ui[$name].Add_SelectionChanged({ Set-Dirty }) }
$ui.PerfCapture.Add_Click({ Set-Dirty })
$ui.Sharpening.Add_Click({ Set-Dirty })
$ui.Save.Add_Click({
    try { Save-SvrSettings $SettingsPath (Read-Controls); Set-Status 'Settings saved. They apply the next time you play.' }
    catch { Set-Status "Could not save settings: $($_.Exception.Message)" $true }
})
$ui.Reset.Add_Click({ Show-Settings (New-SvrSettings); Set-Status 'Defaults restored. Choose Save or Play to keep them.' })
$ui.Logs.Add_Click({
    try {
        $path = if ($script:runPath) { $script:runPath } else { "$projectRoot/analysis" }
        [IO.Directory]::CreateDirectory($path) | Out-Null
        $info = [Diagnostics.ProcessStartInfo]::new($path)
        $info.UseShellExecute = $true
        [Diagnostics.Process]::Start($info) | Out-Null
    } catch { Set-Status "Could not open logs: $($_.Exception.Message)" $true }
})
$ui.Play.Add_Click({
    try {
        if ($SmokeTest) { throw 'Game launch is disabled during launcher checks.' }
        if ($script:busy -or (Get-Process svr2011 -ErrorAction SilentlyContinue)) {
            Set-Status 'The game is already running. Close it normally before starting another session.'
            return
        }
        if (!(Test-Path "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan/svr2011.exe")) {
            throw 'The Vulkan build is missing. Build it with scripts/build.ps1 -Renderer Vulkan.'
        }
        if (!(Test-Path "$projectRoot/assets/default.xex")) { throw 'Game files are missing from the assets folder.' }
        $value = Read-Controls
        Save-SvrSettings $SettingsPath $value
        $script:runSettings = $value
        $script:runPath = "$projectRoot/analysis/vulkan-play-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
        [IO.Directory]::CreateDirectory($script:runPath) | Out-Null
        # Freeze this run's settings; subsequent launcher edits cannot race startup.
        $snapshot = "$script:runPath/settings.json"
        Save-SvrSettings $snapshot $value
        $info = [Diagnostics.ProcessStartInfo]::new("$PSHOME/pwsh.exe")
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.WorkingDirectory = $projectRoot
        foreach ($argument in @('-NoProfile','-NonInteractive','-WindowStyle','Hidden','-File',
                "$PSScriptRoot/Run-LauncherGame.ps1",'-SettingsPath',$snapshot,'-RunDirectory',$script:runPath)) {
            $info.ArgumentList.Add($argument)
        }
        $script:runner = [Diagnostics.Process]::Start($info)
        $script:busy = $true
        $ui.Play.IsEnabled = $false
        $ui.SettingsPanel.IsEnabled = $false
        $ui.Play.Content = 'Game running'
        Set-Status 'Game starting. You can close this launcher; your game will keep running.'
    } catch { Set-Status "Could not start: $($_.Exception.Message)" $true }
})
$timer = [Windows.Threading.DispatcherTimer]::new()
$timer.Interval = [TimeSpan]::FromSeconds(1)
$timer.Add_Tick({
    if ($script:runner -and $script:runner.HasExited) {
        $code = $script:runner.ExitCode
        $script:runner.Dispose()
        $script:runner = $null
        $script:busy = $false
        $ui.SettingsPanel.IsEnabled = $true
        $ui.Play.IsEnabled = $true
        $ui.Play.Content = '_Play SVR 2011'
        $native = $script:runSettings.renderer -eq 'Native'
        # Logged by the game when the native renderer cannot start and Classic takes over; logs rotate.
        $fellBack = $native -and [bool](Select-String -Path "$script:runPath/runtime*.log" -SimpleMatch -Quiet `
            -Pattern 'native renderer: could not create Vulkan resources' -ErrorAction SilentlyContinue)
        if ($code -eq 0 -and $fellBack) { Set-Status 'The native renderer could not start on this PC, so the game used Classic. Open logs for the details.' $true }
        elseif ($code -eq 0) { Set-Status 'Game closed normally. Ready for another match.' }
        elseif ($native -and !$fellBack) { Set-Status 'The game stopped unexpectedly with the native renderer. Switch Renderer to Classic if it happens again, and open logs to report it.' $true }
        else { Set-Status 'The session ended with an error. Open logs for launch-error.txt and runtime.log.' $true }
    }
})
$window.Add_Closed({
    $timer.Stop()
    # Dispose only our handle. Never terminate the observer or the game.
    if ($script:runner) { $script:runner.Dispose() }
})
$window.Add_ContentRendered({
    if ($script:warning) { Set-Status $script:warning $true }
    elseif (!(Test-Path "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan/svr2011.exe")) {
        Set-Status 'Vulkan build not found. Build the project before playing.' $true
        $ui.Play.IsEnabled = $false
    }
    if ($SmokeTest) {
        try {
            Test-SvrSettings (Read-Controls)
            if (!$ui.ScaleHelp.Text -or $ui.WindowSize.IsEnabled) { throw 'Default UI state is incorrect.' }
            Select-Value $ui.DisplayMode 'Windowed'
            if (!$ui.WindowSize.IsEnabled) { throw 'Window-size control did not enable.' }
            Select-Value $ui.Scale 2
            if ($ui.ScaleHelp.Text -notlike '*More detail*') { throw 'Scaling explanation did not update.' }
            if ($ui.RendererHelp.Text -notlike '*Xbox 360*') { throw 'Classic renderer explanation missing.' }
            Select-Value $ui.Renderer 'Native'
            if ($ui.RendererHelp.Text -notlike '*Vulkan*' -or (Read-Controls).renderer -ne 'Native') { throw 'Renderer choice did not update.' }
            Show-Settings (New-SvrSettings)
            Set-Status 'Ready. Saves are kept in userdata/vulkan.'
            if ($ScreenshotPath) {
                $window.UpdateLayout()
                $surface = $window.Content
                $bitmap = [Windows.Media.Imaging.RenderTargetBitmap]::new([int]$surface.ActualWidth, [int]$surface.ActualHeight, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
                $bitmap.Render($surface)
                $encoder = [Windows.Media.Imaging.PngBitmapEncoder]::new()
                $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
                $stream = [IO.File]::Create($ScreenshotPath)
                try { $encoder.Save($stream) } finally { $stream.Dispose() }
            }
        } catch { $script:smokeError = $_ } finally { $window.Close() }
    }
})
$timer.Start()
$null = $window.ShowDialog()
if ($script:smokeError) { throw $script:smokeError }
if ($SmokeTest) { Write-Output 'Launcher UI checks passed.' }
