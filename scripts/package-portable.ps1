# Build the portable player package: launcher, the validated Vulkan build and its runtime.
# Players supply their own disc image; no game data is included.
param(
    [string]$VisualStudioRoot = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
. "$PSScriptRoot/Build-PortableLauncher.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
$build = "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan"
$runtime = @('svr2011.exe', 'rexruntimerd.dll', 'rexgpu-xenosrd.dll', 'TracyClientrd.dll')
foreach ($name in $runtime) {
    if (!(Test-Path -LiteralPath "$build/$name")) { throw "Missing $name. Build first with scripts/build.ps1 -Renderer Vulkan." }
}
$discReport = "$projectRoot/analysis/disc.json"
if (!(Test-Path -LiteralPath $discReport)) { throw 'Missing analysis/disc.json. Run inspect_disc.py --extract-all first.' }
$disc = Get-Content -LiteralPath $discReport -Raw | ConvertFrom-Json
if ($disc.executable.sha256 -ne '6aead4cb29caa11f245d37b36277b5ab57772cffa2cf8c459d3295a3b2d3ebc1' -or
    @($disc.files | Where-Object { !$_.sha256 }).Count) {
    throw 'analysis/disc.json does not describe a fully verified extraction of the supported disc.'
}

$git = (Get-Command git).Source
$commit = (Invoke-Hidden $git @('-C', $projectRoot, 'rev-parse', '--short', 'HEAD') | Out-String).Trim()
$dirty = (Invoke-Hidden $git @('-C', $projectRoot, 'status', '--porcelain') | Out-String).Trim()
$version = "$(Get-Date -Format 'yyyy.MM.dd')-$commit$(if ($dirty) { '-dirty' })"
$portableRoot = "$projectRoot/out/portable"
$stage = "$portableRoot/SVR2011-$version"
$marker = "$stage/.portable-package"
if (Test-Path -LiteralPath $stage) {
    # Replace only a folder this script created earlier.
    if (!(Test-Path -LiteralPath $marker)) { throw "Refusing to replace ${stage}: it was not created by this script." }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
$game = "$stage/game"
New-Item -ItemType Directory -Force $game, "$game/licenses" | Out-Null
Set-Content -LiteralPath $marker -Value $version

# Game runtime exactly as validated. Never copy svr2011.toml: it holds this machine's persisted cvars.
foreach ($name in $runtime) { Copy-Item -LiteralPath "$build/$name" -Destination $game }

# App-local Visual C++ runtime so players need no separate installer.
$redist = Get-ChildItem "$VisualStudioRoot/VC/Redist/MSVC" -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem "$($_.FullName)/x64" -Directory -Filter 'Microsoft.VC*.CRT' } | Select-Object -First 1
if (!$redist) { throw 'Visual C++ redistributable files not found under the Build Tools installation.' }
foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Copy-Item -LiteralPath "$($redist.FullName)/$name" -Destination $game
}

# Expected disc contents let the launcher verify each player's image file by file.
$manifest = @('# sha256<TAB>size<TAB>path for WWE SmackDown vs. Raw 2011 (Xbox 360, USA/Europe)')
$manifest += $disc.files | ForEach-Object { "$($_.sha256)`t$($_.size)`t$($_.path)" }
[IO.File]::WriteAllLines("$game/disc-manifest.tsv", [string[]]$manifest)
Set-Content -LiteralPath "$game/version.txt" -Value "Build $version"

Copy-Item -LiteralPath "$projectRoot/third_party/rexglue-LICENSE.txt" -Destination "$game/licenses/rexglue-LICENSE.txt"
foreach ($directory in Get-ChildItem "$projectRoot/.tools/rexglue-source/thirdparty" -Directory) {
    foreach ($license in Get-ChildItem $directory.FullName -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|LICENCE)' }) {
        Copy-Item -LiteralPath $license.FullName -Destination "$game/licenses/$($directory.Name)-$($license.Name)"
    }
}

# Launcher icon comes from the game executable.
$icon = "$portableRoot/launcher.ico"
Add-Type -AssemblyName System.Drawing
$extracted = [System.Drawing.Icon]::ExtractAssociatedIcon((Resolve-Path "$build/svr2011.exe").Path)
$stream = [IO.File]::Create($icon)
try { $extracted.Save($stream) } finally { $stream.Dispose(); $extracted.Dispose() }
Build-PortableLauncher "$stage/SVR 2011.exe" $icon
Copy-Item -LiteralPath "$projectRoot/launcher/portable/README.txt" -Destination "$stage/README.txt"

# Symbols stay local so crash reports from this exact build can be symbolized.
$symbols = "$portableRoot/symbols-$version"
New-Item -ItemType Directory -Force $symbols | Out-Null
foreach ($name in $runtime) {
    $pdb = [IO.Path]::ChangeExtension("$build/$name", '.pdb')
    if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $symbols -Force }
}

Remove-Item -LiteralPath $marker
if (!$NoZip) {
    $zip = "$stage.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory((Resolve-Path $stage).Path, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    Set-Content -LiteralPath $marker -Value $version
    Write-Output "Portable package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
} else {
    Set-Content -LiteralPath $marker -Value $version
}
Write-Output "Package folder: $stage"
Write-Output "Symbols: $symbols"
