# Fetch the exact SDK sources and their pinned dependencies; install nothing system-wide.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
$source = "$projectRoot/.tools/rexglue-source"
$git = (Get-Command git).Source
New-Item -ItemType Directory -Force "$projectRoot/.tools", "$projectRoot/analysis" | Out-Null
if (!(Test-Path $source)) {
    Invoke-Hidden $git @('clone', '--depth', '1', '--branch', 'v0.10.0', 'https://github.com/rexglue/rexglue-sdk.git', $source)
}
$revision = (Invoke-Hidden $git @('-C', $source, 'rev-parse', 'HEAD') | Out-String).Trim()
if ($revision -ne 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2') { throw "Unexpected ReXGlue source revision: $revision" }
Invoke-Hidden $git @('-C', $source, 'submodule', 'update', '--init', '--depth', '1', '--jobs', '6') -LogPath "$projectRoot/analysis/vulkan-submodules.log"
# Git for Windows may check out symlinks as text. Materialize only these known
# libmspack links, preserving real symlinks and refusing to replace edited files.
$mspack = "$source/thirdparty/libmspack"
foreach ($name in @('cab.h', 'cabd.c', 'lzx.h', 'lzxd.c', 'macros.h',
    'mspack.h', 'mszip.h', 'mszipd.c', 'qtm.h', 'qtmd.c', 'readbits.h', 'readhuff.h', 'system.c', 'system.h')) {
    $destination = "$mspack/cabextract/mspack/$name"
    $target = "$mspack/libmspack/mspack/$name"
    if ((Get-Item -LiteralPath $destination).LinkType) { continue }
    if ((Get-Content -LiteralPath $destination -Raw).Trim() -eq "../../libmspack/mspack/$name") {
        Copy-Item -LiteralPath $target -Destination $destination
    } elseif ((Get-FileHash $destination).Hash -ne (Get-FileHash $target).Hash) {
        throw "Refusing to overwrite modified libmspack file: $destination"
    }
}
Write-Output 'Vulkan SDK sources ready. Run scripts/build.ps1 -Renderer Vulkan -Action Configure, then -Action Build.'
