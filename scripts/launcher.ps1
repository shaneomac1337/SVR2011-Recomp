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
foreach ($name in @('DisplayMode','WindowSize','Scale','ScaleHelp','Presentation','PresentationHelp','Controller','PerfCapture','Save','Reset','Play','Logs','Status','SettingsPanel')) {
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
        'Higher detail uses more GPU resources. Check entrances and finishers; return to native if rendering breaks.'
    } else { 'Native rendering is the validated setting. Window size does not change rendering detail.' }
    $ui.PresentationHelp.Text = switch ($ui.Presentation.SelectedItem.Tag) {
        'Mailbox' { 'Syncs display output without a fixed FPS cap. Falls back to monitor VSync if unavailable. Needs a gameplay check.' }
        'Fifo' { 'Limits display output to the monitor refresh rate, not necessarily 60 Hz. May add latency; check match smoothness.' }
        default { 'Keeps the proven presentation mode. Overlay FPS may exceed the actual game update rate.' }
    }
}
function Show-Settings($Value) {
    $script:loading = $true
    Select-Value $ui.DisplayMode $Value.displayMode
    Select-Value $ui.WindowSize $Value.windowSize
    Select-Value $ui.Scale $Value.scale
    Select-Value $ui.Controller $Value.controller
    Select-Value $ui.Presentation $Value.presentation
    $ui.PerfCapture.IsChecked = $Value.perfCapture
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
    $value.perfCapture = [bool]$ui.PerfCapture.IsChecked
    return $value
}
function Set-Dirty {
    if (!$script:loading) {
        Update-DisplayHelp
        Set-Status 'Unsaved changes. Play also saves your settings.'
    }
}
Show-Settings $script:settings
foreach ($name in @('DisplayMode','WindowSize','Scale','Controller','Presentation')) { $ui[$name].Add_SelectionChanged({ Set-Dirty }) }
$ui.PerfCapture.Add_Click({ Set-Dirty })
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
        if ($code -eq 0) { Set-Status 'Game closed normally. Ready for another match.' }
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
            if ($ui.ScaleHelp.Text -notlike '*Higher detail*') { throw 'Scaling explanation did not update.' }
            Show-Settings (New-SvrSettings)
            Set-Status 'Ready. Your progress stays in the existing save folder.'
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
