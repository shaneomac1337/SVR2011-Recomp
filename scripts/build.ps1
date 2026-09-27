param(
    [ValidateSet('Configure', 'Build', 'Codegen')][string]$Action = 'Build',
    [ValidateSet('D3D12', 'Vulkan')][string]$Renderer = 'D3D12',
    [string]$VisualStudioRoot = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string]$WindowsSdkRoot = 'C:/Program Files (x86)/Windows Kits/10',
    [ValidateRange(1, 64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$expectedXex = '6aead4cb29caa11f245d37b36277b5ab57772cffa2cf8c459d3295a3b2d3ebc1'
if ((Get-FileHash 'assets/default.xex' -Algorithm SHA256).Hash -ne $expectedXex) {
    throw 'Unsupported executable revision. This manifest targets Title 5451085D / Media 4A4F538E.'
}
$sdk = "$projectRoot/.tools/rexglue-0.10.0/win-amd64"
$llvm = "$projectRoot/.tools/llvm-21.1.8/bin"
$cmakeRoot = "$VisualStudioRoot/Common7/IDE/CommonExtensions/Microsoft/CMake"
$cmake = "$cmakeRoot/CMake/bin/cmake.exe"
$msvc = Get-ChildItem "$VisualStudioRoot/VC/Tools/MSVC" -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$windowsVersion = Get-ChildItem "$WindowsSdkRoot/Include" -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
foreach ($path in @("$sdk/bin/rexglue.exe", "$llvm/clang++.exe", $cmake,
    "$cmakeRoot/Ninja/ninja.exe", "$($msvc.FullName)/include/vector",
    "$WindowsSdkRoot/Include/$($windowsVersion.Name)/um/Windows.h")) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing build prerequisite: $path" }
}
$env:PATH = "$llvm;$cmakeRoot/Ninja;$($msvc.FullName)/bin/Hostx64/x64;$WindowsSdkRoot/bin/$($windowsVersion.Name)/x64;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)/include;$WindowsSdkRoot/Include/$($windowsVersion.Name)/ucrt;$WindowsSdkRoot/Include/$($windowsVersion.Name)/shared;$WindowsSdkRoot/Include/$($windowsVersion.Name)/um;$WindowsSdkRoot/Include/$($windowsVersion.Name)/winrt"
$env:LIB = "$($msvc.FullName)/lib/x64;$WindowsSdkRoot/Lib/$($windowsVersion.Name)/ucrt/x64;$WindowsSdkRoot/Lib/$($windowsVersion.Name)/um/x64"
New-Item -ItemType Directory -Force analysis | Out-Null
$preset = 'win-amd64-relwithdebinfo'
$buildDirectory = "$projectRoot/out/build/$preset"
$configureArguments = @('--preset', $preset, "-DCMAKE_PREFIX_PATH=$sdk", '-DREXSDK_VERSION=0.10.0')
$logSuffix = ''
if ($Renderer -eq 'Vulkan') {
    $source = "$projectRoot/.tools/rexglue-source"
    if (!(Test-Path "$source/CMakeLists.txt")) { throw 'Run scripts/prepare-vulkan.ps1 first.' }
    $revision = (Invoke-Hidden (Get-Command git).Source @('-C', $source, 'rev-parse', 'HEAD') | Out-String).Trim()
    if ($revision -ne 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2') { throw "Unexpected ReXGlue source revision: $revision" }
    & "$PSScriptRoot/apply-vulkan-patches.ps1"
    $buildDirectory += '-vulkan'
    $configureArguments += @('-B', $buildDirectory, "-DREXSDK_DIR=$source", '-DREXGLUE_USE_VULKAN=ON', '-DREXGLUE_USE_D3D12=OFF')
    $configureArguments += "-DPYTHON_EXECUTABLE=$((Get-Command python.exe -ErrorAction Stop).Source)"
    # Match the SDK's Windows x64 preset when compiling it from source.
    $configureArguments += @('-DCMAKE_C_FLAGS=-march=x86-64-v2', '-DCMAKE_CXX_FLAGS=-march=x86-64-v2')
    $logSuffix = '-vulkan'
}
if ($Action -eq 'Codegen') {
    Invoke-Hidden "$sdk/bin/rexglue.exe" @('codegen', 'svr2011_manifest.toml', '--ignore-stamp') -LogPath "$projectRoot/analysis/codegen-current.log"
} elseif ($Action -eq 'Configure') {
    Invoke-Hidden $cmake $configureArguments -LogPath "$projectRoot/analysis/configure$logSuffix.log"
} else {
    Invoke-Hidden $cmake @('--build', $buildDirectory, '--parallel', "$Jobs") -LogPath "$projectRoot/analysis/build$logSuffix.log"
    if ($Renderer -eq 'Vulkan') {
        # The SDK's POST_BUILD copy can be skipped when only a runtime DLL changes.
        . "$PSScriptRoot/Sync-VulkanRuntime.ps1"
        Sync-VulkanRuntime "$source/out/win-amd64" $buildDirectory
    }
}
Write-Output "$Action completed. Logs: $projectRoot/analysis/"
