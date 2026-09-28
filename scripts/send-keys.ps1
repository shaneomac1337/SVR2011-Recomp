# Drive a session started with play-vulkan.ps1 -KeyboardInput. Steps are key names (Return, Space,
# Escape, W/A/S/D, Up/Down/Left/Right) with an optional *count, or wait:<ms>.
# Example: scripts/send-keys.ps1 'Return*3','wait:1500','Space'
param([Parameter(Mandatory)][string[]]$Steps, [int]$GapMs = 400)
$ErrorActionPreference = 'Stop'
if (!('SvrKeys' -as [type])) {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class SvrKeys {
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr w);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  static void AltTap() { keybd_event(0x12,0,0,UIntPtr.Zero); keybd_event(0x12,0,2,UIntPtr.Zero); }
  public static void Focus(IntPtr w) {
    // The MnK driver only accepts keys after a focus-change event, so bounce focus via the taskbar.
    AltTap(); SetForegroundWindow(FindWindow("Shell_TrayWnd", null)); Thread.Sleep(150);
    AltTap(); SetForegroundWindow(w); Thread.Sleep(250);
  }
  public static void Tap(byte vk) { keybd_event(vk,0,0,UIntPtr.Zero); Thread.Sleep(90); keybd_event(vk,0,2,UIntPtr.Zero); }
}
'@
}
$codes = @{ Return = 0x0D; Space = 0x20; Escape = 0x1B; Back = 0x08; Tab = 0x09
    Left = 0x25; Up = 0x26; Right = 0x27; Down = 0x28 }
$game = Get-Process svr2011 | Where-Object MainWindowHandle -NE 0 | Select-Object -First 1
if (!$game) { throw 'Game not running.' }
$previous = [SvrKeys]::GetForegroundWindow()
[SvrKeys]::Focus($game.MainWindowHandle)
try {
    foreach ($step in $Steps) {
        if ($step -match '^wait:(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
        if ($step -notmatch '^(\w+)(?:\*(\d+))?$') { throw "Bad step: $step" }
        $name = $Matches[1]; $count = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
        $vk = if ($codes.ContainsKey($name)) { $codes[$name] } elseif ($name.Length -eq 1) { [int][char]$name.ToUpperInvariant() } else { throw "Unknown key: $name" }
        for ($i = 0; $i -lt $count; $i++) { [SvrKeys]::Tap([byte]$vk); Start-Sleep -Milliseconds $GapMs }
    }
} finally { [SvrKeys]::Focus($previous) }
