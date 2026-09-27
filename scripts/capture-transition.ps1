# Capture only the game window, without changing or terminating its process.
param([ValidateRange(2,30)][int]$Seconds = 12, [switch]$WaitForF8)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$references = @('System.Drawing.Common','System.Drawing.Primitives','System.Threading.Thread','System.Diagnostics.TraceSource')
foreach ($name in @('System.Private.Windows.GdiPlus.dll','System.Private.Windows.Core.dll')) {
    if (Test-Path "$PSHOME/$name") { $references += "$PSHOME/$name" }
}
Add-Type -ReferencedAssemblies $references -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
public static class SvrTransitionCapture {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left,Top,Right,Bottom; }
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out RECT rect);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
  [DllImport("user32.dll")] static extern short GetAsyncKeyState(int key);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
  public static void Run(IntPtr window, string directory, int seconds, bool waitForF8) {
    if (waitForF8) {
      while (IsWindow(window) && (GetAsyncKeyState(0x77) & 0x8000) == 0) Thread.Sleep(25);
    }
    if (!IsWindow(window)) throw new InvalidOperationException("The game window closed before recording.");
    Directory.CreateDirectory(directory);
    var timer = Stopwatch.StartNew();
    using (var csv = new StreamWriter(Path.Combine(directory, "frames.csv"))) {
      csv.WriteLine("frame,elapsed_ms,capture_ms,mean_rgb,black_fraction,utc_ms");
      int index = 0;
      while (timer.Elapsed.TotalSeconds < seconds && IsWindow(window)) {
        long start = timer.ElapsedMilliseconds;
        long utcMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        RECT rect;
        if (!GetWindowRect(window, out rect)) break;
        int width = rect.Right - rect.Left, height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0) break;
        using (var full = new Bitmap(width, height)) {
          bool captured;
          using (var graphics = Graphics.FromImage(full)) {
            IntPtr dc = graphics.GetHdc();
            try { captured = PrintWindow(window, dc, 2); }
            finally { graphics.ReleaseHdc(dc); }
          }
          if (!captured) throw new InvalidOperationException("Game-window capture failed.");
          using (var preview = new Bitmap(640, 360)) {
            using (var graphics = Graphics.FromImage(preview)) graphics.DrawImage(full, 0, 0, 640, 360);
            long sum = 0; int black = 0, samples = 0;
            // Exclude borders and the external overlay when classifying a frame.
            for (int y = 40; y < 330; y += 8) for (int x = 80; x < 600; x += 8) {
              var pixel = preview.GetPixel(x,y);
              int intensity = (pixel.R + pixel.G + pixel.B) / 3;
              sum += intensity; if (intensity < 12) black++; samples++;
            }
            preview.Save(Path.Combine(directory, index.ToString("D5") + ".jpg"), ImageFormat.Jpeg);
            csv.WriteLine(string.Format(CultureInfo.InvariantCulture, "{0},{1},{2},{3:F3},{4:F4},{5}",
              index, start, timer.ElapsedMilliseconds-start, (double)sum/samples, (double)black/samples, utcMs));
          }
        }
        index++;
        long delay = index * 1000L / 60 - timer.ElapsedMilliseconds;
        if (delay > 0) Thread.Sleep((int)delay);
      }
    }
  }
}
'@
$game = Get-Process svr2011 -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -NE 0 | Select-Object -First 1
if (!$game) { throw 'Start the game before capturing a transition.' }
$root = Split-Path $PSScriptRoot -Parent
$directory = "$root/analysis/transition-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
Write-Output "Recording folder: $directory"
if ($WaitForF8) { Write-Output 'Press F8 in the game, then reproduce the character-select flash. Recording ends automatically; the game stays open.' }
[SvrTransitionCapture]::Run($game.MainWindowHandle, $directory, $Seconds, [bool]$WaitForF8)
Write-Output "Capture finished: $directory. The game has not been stopped."
