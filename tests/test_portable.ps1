# Checks the portable launcher without opening its window or starting the game.
param([string]$Iso = "$PSScriptRoot/../WWE SmackDown vs. Raw 2011 (USA, Europe).iso")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Launcher.Core.ps1"
. "$PSScriptRoot/../scripts/Build-PortableLauncher.ps1"
function Assert($Value, [string]$Message) { if (!$Value) { throw $Message } }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$launcher = Join-Path $scratch 'launcher.exe'
function Invoke-Check([string[]]$Arguments) {
    $output = Join-Path $scratch 'check.txt'
    $info = [Diagnostics.ProcessStartInfo]::new($launcher)
    foreach ($argument in @($Arguments + $output)) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        if (!$process.WaitForExit(120000)) { throw 'Launcher check did not finish.' }
        [pscustomobject]@{ Code = $process.ExitCode; Text = [IO.File]::ReadAllText($output) }
    } finally { $process.Dispose() }
}
try {
    Build-PortableLauncher $launcher

    # Display arguments must match the development launcher for every option.
    $settingsPath = Join-Path $scratch 'settings.json'
    foreach ($case in @(
        @{}, @{ displayMode = 'Windowed'; windowSize = '1920x1080'; scale = 3; controller = 'Xbox'; perfCapture = $true },
        @{ presentation = 'Mailbox'; scale = 2 }, @{ presentation = 'Fifo'; windowSize = '1600x900' })) {
        $settings = New-SvrSettings
        foreach ($key in $case.Keys) { $settings[$key] = $case[$key] }
        Save-SvrSettings $settingsPath $settings
        $result = Invoke-Check @('--print-arguments', $settingsPath)
        Assert ($result.Code -eq 0) "Argument check failed: $($result.Text)"
        $portable = $result.Text -split "`n"
        $expected = @(Get-SvrDisplayArguments $settings)
        $start = [array]::IndexOf($portable, $expected[0])
        Assert ($start -ge 0) 'Display arguments missing.'
        Assert (($portable[$start..($start + $expected.Count - 1)] -join ' ') -eq ($expected -join ' ')) 'Display arguments differ from the development launcher.'
        foreach ($required in @('--render_target_path_vulkan=fsi', '--gpu_allow_invalid_fetch_constants=true', '--gpu_plugin=xenos', '--vulkan_async_skip_placeholder_pipelines=true')) {
            Assert ($portable -contains $required) "Missing runtime argument: $required"
        }
        Assert (($portable -like '--perf_log_csv=*').Count -eq [int]$settings.perfCapture) 'Frame capture argument mismatch.'
    }
    # Older settings without presentation load; invalid values are rejected.
    $old = New-SvrSettings
    $old.Remove('presentation')
    $old | ConvertTo-Json | Set-Content -LiteralPath $settingsPath
    Assert ((Invoke-Check @('--print-arguments', $settingsPath)).Code -eq 0) 'Old settings were rejected.'
    '{"version":1,"displayMode":"Borderless","windowSize":"1280x720","scale":"2","controller":"Auto","perfCapture":false}' |
        Set-Content -LiteralPath $settingsPath
    Assert ((Invoke-Check @('--print-arguments', $settingsPath)).Code -eq 1) 'String scale was accepted.'

    # Quoting must survive CommandLineToArgvW for paths with spaces and trailing backslashes.
    foreach ($case in @(
        @('plain', 'plain'), @('--root=C:\My Games\SVR', '"--root=C:\My Games\SVR"'),
        @('C:\a b\', '"C:\a b\\"'), @('say "hi"', '"say \"hi\""'))) {
        $result = Invoke-Check @('--quote', $case[0])
        Assert ($result.Text -eq $case[1]) "Quote of '$($case[0])' was $($result.Text)"
    }

    # Synthetic disc: nested directory, verified extraction, mismatch detection.
    $disc = Join-Path $scratch 'disc.iso'
    $manifest = Join-Path $scratch 'manifest.tsv'
    $python = @'
import hashlib, struct, sys
from scripts.inspect_disc import MAGIC, SECTOR
data = bytearray(48 * SECTOR)
data[0x10000:0x10014] = MAGIC
data[0x10800 - 20:0x10800] = MAGIC
struct.pack_into("<II", data, 0x10014, 34, SECTOR)
def entry(sector, offset, left, target, length, attributes, name):
    struct.pack_into("<HHIIBB", data, sector * SECTOR + offset, left, 0, target, length, attributes, len(name))
    data[sector * SECTOR + offset + 14:sector * SECTOR + offset + 14 + len(name)] = name
entry(34, 0, 8, 36, 4, 0, b"default.xex")
entry(34, 32, 0, 35, SECTOR, 0x10, b"media")
entry(35, 0, 0, 37, 5, 0, b"arena.pak")
data[36 * SECTOR:36 * SECTOR + 4] = b"XEX2"
data[37 * SECTOR:37 * SECTOR + 5] = b"arena"
open(sys.argv[1], "wb").write(data)
with open(sys.argv[2], "w", newline="\n") as out:
    out.write("# test\n")
    for path, body in (("default.xex", b"XEX2"), ("media/arena.pak", b"arena")):
        out.write(f"{hashlib.sha256(body).hexdigest()}\t{len(body)}\t{path}\n")
'@
    Push-Location "$PSScriptRoot/.."
    try { $python | python - $disc $manifest; Assert ($LASTEXITCODE -eq 0) 'Could not create test disc.' } finally { Pop-Location }
    Assert ((Invoke-Check @('--verify-disc', $disc, $manifest)).Text -eq 'ok') 'Matching disc was rejected.'
    $target = Join-Path $scratch 'gamedata'
    $result = Invoke-Check @('--extract', $disc, $manifest, $target)
    Assert ($result.Text -eq 'ok') "Extraction failed: $($result.Text)"
    Assert ([IO.File]::ReadAllText("$target/media/arena.pak") -eq 'arena') 'Nested file was not extracted.'
    $result = Invoke-Check @('--extract', $disc, $manifest, $target)
    Assert ($result.Code -eq 1) 'Extraction overwrote existing files.'
    Assert ([IO.File]::ReadAllText("$target/default.xex") -eq 'XEX2') 'Existing file was modified.'
    $lines = Get-Content -LiteralPath $manifest
    $lines[2] = $lines[2] -replace '^\w+', ('0' * 64)
    Set-Content -LiteralPath $manifest -Value $lines
    Assert ((Invoke-Check @('--extract', $disc, $manifest, (Join-Path $scratch 'bad'))).Text -like 'error:*failed verification*') 'Corrupt file passed verification.'
    Set-Content -LiteralPath $manifest -Value ($lines[0..1])
    Assert ((Invoke-Check @('--verify-disc', $disc, $manifest)).Text -like 'error:*not the supported version*') 'Different disc was accepted.'
    Assert ((Invoke-Check @('--verify-disc', $manifest, $manifest)).Text -like 'error:*not an Xbox 360 disc*') 'Non-disc file was accepted.'

    # The real disc, when present, must match the manifest the package ships.
    if (Test-Path -LiteralPath $Iso) {
        $disc = Get-Content "$PSScriptRoot/../analysis/disc.json" -Raw | ConvertFrom-Json
        [IO.File]::WriteAllLines($manifest, [string[]]($disc.files | ForEach-Object { "$($_.sha256)`t$($_.size)`t$($_.path)" }))
        $result = Invoke-Check @('--verify-disc', (Resolve-Path $Iso).Path, $manifest)
        Assert ($result.Text -eq 'ok') "Supported disc was rejected: $($result.Text)"
    } else { Write-Output 'Real disc image not found; skipped its check.' }
    Write-Output 'Portable launcher checks passed.'
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
