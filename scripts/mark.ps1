# Record "something happens now" in the newest hitch session, e.g. scripts/mark.ps1 finisher
param([string]$Label = 'mark', [string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$RunDirectory) {
    $RunDirectory = Get-ChildItem "$projectRoot/analysis" -Directory -Filter 'hitch-*' |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
}
if (!$RunDirectory) { throw 'No hitch session found. Start one with scripts/hitch-session.ps1.' }
$line = "$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff'))`t$Label"
Add-Content -LiteralPath (Join-Path $RunDirectory 'marks.txt') -Value $line
Write-Output "Marked in $(Split-Path $RunDirectory -Leaf): $line"
