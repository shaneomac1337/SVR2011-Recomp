# Create a double-click entry point without opening a terminal.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!(Test-Path "$PSHOME/pwsh.exe")) { throw 'Run this installer from PowerShell 7.' }
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut("$projectRoot/SVR 2011.lnk")
$shortcut.TargetPath = "$PSHOME/pwsh.exe"
$shortcut.Arguments = "-NoProfile -STA -WindowStyle Hidden -File `"$PSScriptRoot/launcher.ps1`""
$shortcut.WorkingDirectory = $projectRoot
$shortcut.Description = 'Play SVR 2011 and configure graphics settings'
$shortcut.IconLocation = "$projectRoot/out/build/win-amd64-relwithdebinfo-vulkan/svr2011.exe,0"
$shortcut.Save()
Write-Output "Launcher shortcut: $projectRoot/SVR 2011.lnk"
