param([Parameter(Mandatory)][string]$SettingsPath, [Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
try {
    & "$PSScriptRoot/play-vulkan.ps1" -SettingsPath $SettingsPath -RunDirectory $RunDirectory
} catch {
    $_ | Out-String | Set-Content -LiteralPath "$RunDirectory/launch-error.txt"
    exit 1
}
