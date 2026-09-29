$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Invoke-Hidden.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
$source = "$projectRoot/.tools/rexglue-source"
$git = (Get-Command git).Source
$revision = (Invoke-Hidden $git @('-C', $source, 'rev-parse', 'HEAD') | Out-String).Trim()
if ($revision -ne 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2') { throw "Unexpected SDK revision: $revision" }
foreach ($patchName in @('rexglue-window-restore.patch', 'rexglue-async-pipelines.patch', 'rexglue-xmp-no-delay.patch', 'rexglue-frame-pacing.patch', 'rexglue-resolve-readback.patch', 'rexglue-draw-census.patch')) {
$patch = "$projectRoot/patches/$patchName"
$applied = $false
try {
    $null = Invoke-Hidden $git @('-C', $source, 'apply', '--reverse', '--check', $patch)
    $applied = $true
} catch { }
if (!$applied) {
    $null = Invoke-Hidden $git @('-C', $source, 'apply', '--check', $patch)
    $null = Invoke-Hidden $git @('-C', $source, 'apply', $patch)
}
}
