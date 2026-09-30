# Convert the shader containers captured with --svr_shader_dump_dir into the
# native renderer's SPIR-V cache (cache/shader-native/shader_cache.cpp), which
# the next build compiles in. Both folders hold game-derived data and stay local.
param([string]$InputDirectory = 'cache/guest-shaders')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$tool = 'out/build/xenosrecomp/XenosRecomp/XenosRecomp.exe'
if (!(Test-Path $tool)) { & "$PSScriptRoot/build-xenosrecomp.ps1" | Out-Null }
if (!(Get-ChildItem $InputDirectory -Filter '*.bin' -ErrorAction SilentlyContinue)) {
    throw "No shader containers in $InputDirectory. Run the game once with --svr_shader_dump_dir=$InputDirectory."
}
New-Item -ItemType Directory -Force cache/shader-native | Out-Null
& $tool $InputDirectory cache/shader-native/shader_cache.cpp .tools/xenosrecomp/XenosRecomp/shader_common.h cache/shader-native/hlsl *> cache/shader-native/run.log
if ($LASTEXITCODE -ne 0) { throw "XenosRecomp failed; see cache/shader-native/run.log" }
# The texture slots each shader reads, so draws bind only those.
& python "$PSScriptRoot/shader_fetch_slots.py" cache/shader-native/hlsl cache/shader-native/fetch_slots.cpp
if ($LASTEXITCODE -ne 0) { throw "shader_fetch_slots.py failed" }
Write-Output "Shader cache written: cache/shader-native/shader_cache.cpp and fetch_slots.cpp"
