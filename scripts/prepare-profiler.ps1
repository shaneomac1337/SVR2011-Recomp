$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$archive = "$projectRoot/.tools/tracy-windows-0.13.1.zip"
New-Item -ItemType Directory -Force "$projectRoot/.tools" | Out-Null
if (!(Test-Path $archive)) {
    Invoke-WebRequest 'https://github.com/wolfpld/tracy/releases/download/v0.13.1/windows-0.13.1.zip' -OutFile $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne 'ee6db1a7e71a12deb5973a8dbfdf9f36d3635bec0e0b31b1cc74f28de7dac4c9') {
    throw 'Tracy archive checksum mismatch'
}
Expand-Archive -LiteralPath $archive -DestinationPath "$projectRoot/.tools/tracy-0.13.1" -Force
Write-Output 'Tracy 0.13.1 ready; matches the pinned SDK profiler version.'
