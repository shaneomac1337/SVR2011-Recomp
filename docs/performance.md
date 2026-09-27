# One on One performance and rendering investigation

Scope: make the Orton–Mysterio One on One path dependable and smooth, including
entrances, finishers, victory and returning from the match. The user completed
one match, but this is not yet a production-ready port. Arena is not recorded.
Never terminate an interactive test or restart it automatically. The user owns
the session lifetime.

## Baseline

`analysis/vulkan-play-20260927-184353/` contains the first user-confirmed completed
match. The picture went black during cinematic/finisher sequences, then returned
afterward. Audio behavior during black screens has not been confirmed. At the
end of the run, the log records missing target `0x82600ED0`; that original
five-instruction dispatch thunk is now registered and the build passes. It is
adjacent to `0x82600EB8`. Runtime validation of this latest seed is pending.

The capture has 12,000 rows. A 100-second interval (70–170 seconds after the first
measured guest swap) has 5,999 frames, 59.992 guest FPS, median 16.533 ms,
95th percentile 18.042 ms, 99th percentile 18.542 ms, maximum 33.115 ms.
There are no >33.333 ms intervals in that selection. Earlier ten-second windows
range around 41–46 FPS; later windows drop to 45–52 FPS, with a final partial
window near 30. Without scene markers these ranges cannot be assigned to exact
animations. Whole-run averages must not be used as evidence of steady gameplay.

Reproduce the selected measurement after the game exits:

```powershell
python scripts/analyze_perf.py analysis/vulkan-play-20260927-184353/perf.csv --start 70 --end 170
```

This measures guest swap submissions, not simulation correctness, GPU execution
time, or displayed frames. The external 60 FPS overlay is not an adequate
performance signal. The optional 60 FPS target in the analyzer only defines the
two-frame-budget threshold; it does not assert every cinematic should be 60 FPS.
CSV writes flush periodically; crashes may lose the last buffered rows.

## Current comparison: rejected texture descriptors

Ranked hypotheses:

1. Post-processing samples textures rejected as `kInvalidTexture`, producing
   black cinematic frames. Prediction: accepting that specific descriptor type
   restores those same animations; if they remain black, the hypothesis is
   insufficient.
2. New pipeline/shader compilation stalls scene changes. Prediction: repeating
   the same scene with a warm cache reduces its worst frame intervals; a CPU
   profile should attribute corresponding stalls to compilation/waiting.
3. Frame pacing, CPU runtime work or synchronization causes sustained slowdown.
   Prediction: problematic frames contain measurable CPU zones or waits rather
   than only isolated compiler activity.

`TextureCache::BindingInfoFromFetchConstant` explicitly rejects the first type
unless `gpu_allow_invalid_fetch_constants` is set. The current logs contain many
of these rejections. This establishes a plausible rendering path, not a proven
black-screen cause. The option still rejects completely invalid descriptor types
and zero-data textures. Baseline remains the default.

```powershell
./scripts/play-vulkan.ps1 -PerfCapture -Experiment InvalidFetch
```

Compare the same match, arena, settings, entrance, finisher and victory. Repeat
once warm before claiming a speed improvement. Record visibility as well as
timings; an FPS increase achieved by skipping rendering is not a pass. Close the
game manually afterward. `launch.json` records the exact arguments, and
`result.json` records the exit. Return to the baseline with `-Experiment Baseline`.
This experiment has been prepared but not yet validated in gameplay.

## Deeper profiling

The local [Tracy 0.13.1 release](https://github.com/wolfpld/tracy/releases/tag/v0.13.1)
matches the SDK client. Its official archive is SHA-256 checked; nothing is
installed system-wide. Attach in a separate PowerShell session while exactly one
game is running:

```powershell
./scripts/prepare-profiler.ps1
./scripts/capture-profile.ps1 -Seconds 30
```

Only the capture ends after 30 seconds. This script neither starts nor stops the
game. Output is `analysis/profile-<timestamp>/session.tracy` plus per-zone
self-time statistics. Profiling adds overhead: run this separately from the
unprofiled Baseline/InvalidFetch timing comparison. Attachment and data export
still require an end-to-end runtime check.

## Acceptance before calling this path ready

- Repeated full One on One matches complete and return to the menu without fatal
  callbacks, device loss, hangs or abnormal exits.
- Entrances, finisher effects and victory animations remain visible.
- Interactive match output sustains the intended 60 FPS with correct simulation
  speed, stable frame intervals, and no recurring multi-frame stalls on a warm
  cache. Native cinematic frame caps are assessed separately.
- Cold-cache shader behavior is measured and improved without hiding draws.
- Results record GPU/driver, build, configuration, arena and cache state. Test
  more than one run before promoting an experimental setting to the default.
