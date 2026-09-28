# Character-select flicker check; needs play-vulkan.ps1 -KeyboardInput and the roster cursor active.
# Taps D/A and samples the window: exit 1 on dark runs of 3+ frames, 0 otherwise. Never stops the game.
param([int]$Switches = 6, [int]$IntervalMs = 900, [string]$OutDir)
$ErrorActionPreference = 'Stop'
$refs = @('System.Drawing.Common','System.Drawing.Primitives','System.Threading.Thread','System.Collections')
foreach ($n in @('System.Private.Windows.GdiPlus.dll','System.Private.Windows.Core.dll')) { if (Test-Path "$PSHOME/$n") { $refs += "$PSHOME/$n" } }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies $refs -TypeDefinition @'
using System; using System.Collections.Generic; using System.Diagnostics; using System.Drawing;
using System.Drawing.Imaging; using System.IO; using System.Runtime.InteropServices; using System.Threading;
public static class FlickerLoop {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w, IntPtr dc, uint f);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr w);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public static void Focus(IntPtr w) {
    keybd_event(0x12,0,0,UIntPtr.Zero); keybd_event(0x12,0,2,UIntPtr.Zero); // Alt tap unlocks SetForegroundWindow
    // The MnK driver only accepts keys after a focus-change event, so bounce focus via the taskbar.
    SetForegroundWindow(FindWindow("Shell_TrayWnd", null)); Thread.Sleep(150);
    keybd_event(0x12,0,0,UIntPtr.Zero); keybd_event(0x12,0,2,UIntPtr.Zero);
    SetForegroundWindow(w); Thread.Sleep(250);
  }
  static void Tap(byte vk) { keybd_event(vk,0,0,UIntPtr.Zero); Thread.Sleep(90); keybd_event(vk,0,2,UIntPtr.Zero); }
  // Rows: elapsed_ms, mean brightness, key sent (0/1), unix_ms.
  public static List<double[]> Run(IntPtr w, string dir, int switches, int intervalMs) {
    var rows = new List<double[]>(); var t = Stopwatch.StartNew();
    long nextKey = 700; int sent = 0; long end = 700 + (long)switches*intervalMs + 900; int i = 0;
    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
    while (t.ElapsedMilliseconds < end) {
      double key = 0;
      if (sent < switches && t.ElapsedMilliseconds >= nextKey) {
        byte vk = (byte)(sent % 2 == 0 ? 0x44 : 0x41); new Thread(() => Tap(vk)).Start();
        sent++; nextKey += intervalMs; key = 1;
      }
      long start = t.ElapsedMilliseconds; RECT r; GetWindowRect(w, out r);
      using (var full = new Bitmap(r.R-r.L, r.B-r.T)) {
        using (var g = Graphics.FromImage(full)) { var dc=g.GetHdc(); PrintWindow(w,dc,2); g.ReleaseHdc(dc); }
        using (var p = new Bitmap(640,360)) {
          using (var g = Graphics.FromImage(p)) g.DrawImage(full,0,0,640,360);
          long sum=0; int n=0;
          for (int y=40;y<330;y+=8) for (int x=80;x<600;x+=8){var c=p.GetPixel(x,y); sum+=(c.R+c.G+c.B)/3; n++;}
          rows.Add(new double[]{start,(double)sum/n,key,(double)DateTimeOffset.Now.ToUnixTimeMilliseconds()});
          if (!string.IsNullOrEmpty(dir)) p.Save(Path.Combine(dir, i.ToString("D5")+".jpg"), ImageFormat.Jpeg);
        }
      }
      i++; long delay = i*1000L/60 - t.ElapsedMilliseconds; if (delay>0) Thread.Sleep((int)delay);
    }
    return rows;
  }
}
'@
$game = Get-Process svr2011 | Where-Object MainWindowHandle -NE 0 | Select-Object -First 1
if (!$game) { throw 'Game not running.' }
$previous = [FlickerLoop]::GetForegroundWindow()
[FlickerLoop]::Focus($game.MainWindowHandle)
try { $rows = [FlickerLoop]::Run($game.MainWindowHandle, $(if ($OutDir) { $OutDir } else { $null }), $Switches, $IntervalMs) }
finally { [FlickerLoop]::Focus($previous) }
$means = $rows | ForEach-Object { $_[1] } | Sort-Object
$baseline = $means[[int]($means.Count * 0.75)]
$runs = @(); $start = -1
for ($k = 0; $k -lt $rows.Count; $k++) {
    $dark = $rows[$k][1] -lt 0.65 * $baseline
    if ($dark -and $start -lt 0) { $start = $k }
    if ((-not $dark -or $k -eq $rows.Count - 1) -and $start -ge 0) {
        $len = $k - $start
        if ($len -ge 3) {
            $keyRow = ($rows[0..$start] | Where-Object { $_[2] -eq 1 } | Select-Object -Last 1)
            $runs += [pscustomobject]@{ frame = $start; frames = $len; ms = [int]($rows[$k][0] - $rows[$start][0])
                key_to_dark_ms = if ($keyRow) { [int]($rows[$start][0] - $keyRow[0]) } else { $null }
                start_local = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$rows[$start][3]).LocalDateTime.ToString('HH:mm:ss.fff')
                end_local = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$rows[$k][3]).LocalDateTime.ToString('HH:mm:ss.fff') }
        }
        $start = -1
    }
}
$fps = [math]::Round($rows.Count / (($rows[-1][0] - $rows[0][0]) / 1000), 1)
"samples=$($rows.Count) fps=$fps baseline=$([math]::Round($baseline,1)) switches=$Switches flashes=$($runs.Count)"
$runs | Format-Table -AutoSize | Out-String
if ($runs.Count -gt 0) { 'RED: scene-dark flicker reproduced'; exit 1 } else { 'GREEN: no flicker'; exit 0 }
