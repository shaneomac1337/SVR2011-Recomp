param([string]$SevenZipPath = 'D:/tools/7-Zip/7z.exe')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
New-Item -ItemType Directory -Force .tools | Out-Null
$artifacts = @(
    @{
        File = 'rexglue-sdk-0.10.0-win-amd64.zip'
        Url = 'https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip'
        Sha256 = 'ad4485da853fbe932e9f5dbb643f9b5fe85dc73775ebbad4df6879fe6df0c11b'
        Destination = '.tools/rexglue-0.10.0'
        Executable = '.tools/rexglue-0.10.0/win-amd64/bin/rexglue.exe'
    },
    @{
        File = 'LLVM-21.1.8-win64.exe'
        Url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-win64.exe'
        Sha256 = '7a5386c26497db1691f320121e5b113364dd0274b98e55f15f4dbc00c0450113'
        Destination = '.tools/llvm-21.1.8'
        Executable = '.tools/llvm-21.1.8/bin/clang++.exe'
    }
)
foreach ($artifact in $artifacts) {
    $archive = Join-Path '.tools' $artifact.File
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest $artifact.Url -OutFile $archive
    }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $artifact.Sha256) {
        throw "Checksum mismatch: $archive. No files were extracted."
    }
    if (!(Test-Path -LiteralPath $artifact.Executable)) {
        if ($archive.EndsWith('.zip')) {
            Expand-Archive $archive $artifact.Destination
        } else {
            if (!(Test-Path -LiteralPath $SevenZipPath)) { throw 'Pass -SevenZipPath with the path to 7z.exe.' }
            Invoke-Hidden $SevenZipPath @('x', $archive, "-o$($artifact.Destination)", '-y')
        }
    }
    Write-Output "Verified $($artifact.File)"
}
Write-Output 'Local toolchain is ready. No system installation or PATH changes were made.'
