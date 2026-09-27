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
adjacent to `0x82600EB8`. The subsequent 598.52-second run exited normally,
with no fatal guest targets.

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
black-screen cause by itself. The option still rejects completely invalid
descriptor types and zero-data textures. In the actual comparison run
`analysis/vulkan-play-20260927-185531`, the user confirmed that entrances,
finishers and cutscenes became visible. InvalidFetch is now the provisional
launcher default to retain that rendering fix; repeated validation is pending.

```powershell
./scripts/play-vulkan.ps1 -PerfCapture -Experiment InvalidFetch
```

Compare the same match, arena, settings, entrance, finisher and victory. Repeat
once warm before claiming a speed improvement. Record visibility as well as
timings; an FPS increase achieved by skipping rendering is not a pass. Close the
game manually afterward. `launch.json` records the exact arguments, and
`result.json` records the exit. Return to the baseline with `-Experiment Baseline`.
The comparison exited normally after 598.52 seconds. Its 20,459 valid intervals
cover 596.9604 seconds: median 33.032 ms, p95 35.529 ms, p99 44.503 ms,
maximum 82.651 ms. This mixed-scene capture is not evidence of a speedup.
Ten-second windows initially range around 40 FPS and later settle at exactly
30 FPS; scene markers are unavailable. The user reports a slow background fight
and stuttering during otherwise playable match action, despite a stable
Afterburner presentation graph. Guest swaps and displayed refreshes must be
assessed separately.

## Deeper profiling

The local [Tracy 0.13.1 release](https://github.com/wolfpld/tracy/releases/tag/v0.13.1)
matches the SDK client. Its official archive is SHA-256 checked; nothing is
installed system-wide. Attach in a separate PowerShell session while exactly one
game is running:

```powershell
./scripts/prepare-profiler.ps1
./scripts/play-vulkan.ps1 -PerfCapture -Profile
# In a separate session, once the target scene is visible:
./scripts/capture-profile.ps1 -Seconds 30
```

Only the capture ends after 30 seconds. This script neither starts nor stops the
game. Output is `analysis/profile-<timestamp>/session.tracy` plus per-zone
self-time statistics. Profiling adds overhead: run this separately from the
unprofiled Baseline/InvalidFetch timing comparison. The SDK uses delayed manual
Tracy initialization but did not call Startup. The app now starts Tracy before
guest execution only with `--svr_profile=true`; SDK runtime shutdown already
stops it. The capture script checks for the game's listener before attaching.

End-to-end attachment and CSV export passed in
`analysis/profile-20260927-191123/` (30.07 seconds, 4,915,043 zones).
This initial trace includes the title screen and cannot diagnose the reported
background-fight slowdown. A previous attachment attempt against the old build
never connected; the 18:55:31 rendering comparison therefore remained unprofiled.
The runtime selects the AMD RX 7900 XT Vulkan device, so software rendering is
not the explanation. CPU waits, presentation pacing and GPU execution still
need to be distinguished in a capture of the affected scene.

## Background-fight synchronization comparison (19:20)

The user confirmed the scene before capture. Screenshot
`analysis/background-profile-scene.png` shows Cena–Orton in the menu practice
ring and an external presentation counter of 342 FPS. Trace
`analysis/profile-20260927-192042/session.tracy` covers 30.09 seconds. Its CPU
zone export contains 1,297 guest swaps, with 1,296 inter-swap intervals averaging
23.265 ms (about 43 FPS), median 17.507 ms, p95 34.551 ms, maximum 36.203 ms.
The Vulkan swap CPU zone averages 0.226 ms and peaks at 0.939 ms. These are CPU
durations, not GPU timestamp measurements.

The command processor spends 13.581 aggregate seconds waiting for commands and
12.148 seconds in WAIT_REG_MEM. Of 11,606 WAIT_REG_MEM events, most return
immediately, but p95 is 10.166 ms and maximum 20.000 ms. This makes the SDK's
millisecond polling sleeps a candidate for missed frame deadlines. It does not
prove those waits are unnecessary: guest synchronization may require them.

An optional 1 ms polling-cap patch compiled, but was never linked or tested in
the game. It was deferred after the user reported smooth gameplay from disabling
the RivaTuner limiter. SDK source and the launcher were restored; the candidate
patch is retained only in `analysis/rexglue-wait-polling-deferred.patch`.
The measured improvement therefore cannot be attributed to that patch.
Do not use `vsync=false` as a presentation-only test: that also changes guest
vblank from the configured video rate to 1,000 Hz in this SDK.

## Smooth gameplay after disabling the external limiter

The user closed `analysis/vulkan-play-20260927-191954/` normally (603.88 seconds,
exit code zero, no fatal guest targets) and reported that actual gameplay became
very smooth after turning off the RivaTuner limit. The precise limiter-toggle
time was not recorded. The background screenshot already shows 342 presentation
FPS, so the background's roughly 43 guest FPS must not simply be blamed on the
old 60 FPS limiter; whether that scene still feels slow needs confirmation.

The later 180–600-second interval contains 25,201 frames over 420.0025 seconds:
60.002 guest FPS, median 16.553 ms, p95 18.499 ms, p99 19.012 ms and maximum
23.096 ms. There are no intervals over 33.333 ms. Scene boundaries were not
recorded, but the long steady interval supports the user's smooth-match report.
Tracy was connected only for the earlier background capture, not this interval.

```powershell
python scripts/analyze_perf.py analysis/vulkan-play-20260927-191954/perf.csv --start 180 --end 600
```

Current working setup: Vulkan on RX 7900 XT, texture compatibility enabled,
RivaTuner frame limiter disabled, original guest timing. Ordinary play does not
need `-Profile`. This is a user-observed limiter interaction, not proof of the
exact interception/presentation mechanism. Repeated match and cinematic testing
remains necessary before calling the port production-ready.

## Display synchronization and character-select flicker

The user reports extremely smooth One on One gameplay through the launcher,
but character select still has a brief black flicker. The latest native-scale
launcher session `analysis/vulkan-play-20260927-195330-749/` exited normally after
245.34 seconds with no fatal guest targets.

The launcher now exposes existing Vulkan present-mode controls: Immediate
(default, unchanged), Mailbox (FIFO fallback) and FIFO (monitor refresh rate).
They do not change guest vblank timing. Both synchronized alternatives require
gameplay comparison before promotion. A numeric host frame cap remains future
work: it must avoid blocking guest frame production as the external limiter did.

Flicker investigation, ranked hypotheses:

1. Async shader placeholders or incomplete frames on character transitions.
   Compare identical transitions with async compilation off; expect the flash
   to disappear if this is responsible, potentially replaced by loading pauses.
2. A presentation/swap-source transition briefly displays a blank image.
   If sync shaders does not change the flash, capture the affected transition
   and inspect swaps and source textures before changing presentation behavior.
3. A guest transition or resource update intentionally or incorrectly emits
   black. Compare the same transition on the emulator; similarity alone does
   not establish intended behavior.

`analysis/vulkan-play-20260927-200133-953/` is the first controlled synchronous
shader test, using saved native settings and frame capture. The temporary
`-SynchronousShaders` script switch adds only `--async_shader_compilation=false`;
it is not a persistent launcher default. User confirmation of screen scope,
trigger, flicker outcome and smoothness is pending. Do not label this fixed.

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
