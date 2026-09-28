# One on One performance and rendering investigation

Status (2026-09-28):

- One on One matches, menus and backstage brawls run at 60 FPS on Vulkan with
  the FSI render-target path.
- The character-select flicker is fixed by FSI (see "Resolved: render-target path").
- The 42–46 FPS menus and backstage are fixed by the XMP patch (see "Backstage
  brawl frame pacing").
- Open: other match types are untested, first-seen shaders still cause brief
  hitches, and the async pipeline switch has not been validated.

This file is a dated log; earlier sections are kept as written. Capture folders,
screenshots and helper scripts under `analysis/` are local to the test PC
(Windows 11, RX 7900 XT) and are not in the repository.

Scope: make the Orton–Mysterio One on One path dependable and smooth, including
entrances, finishers, victory and returning from the match. One match was
completed in play testing, but this is not yet a production-ready port. Arena is
not recorded. Interactive runs have no timeout; the player closes the game.

## Baseline

`analysis/vulkan-play-20260927-184353/` contains the first completed match
confirmed in play testing. The picture went black during cinematic/finisher sequences, then returned
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
`analysis/vulkan-play-20260927-185531`, play testing confirmed that entrances,
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
30 FPS; scene markers are unavailable. In play testing the background fight was
slow and otherwise playable match action stuttered, despite a stable
Afterburner presentation graph. Guest swaps and displayed refreshes must be
assessed separately.

Resolved: the match stutter in "Smooth gameplay after disabling the external
limiter"; the slow background fight in "Backstage brawl frame pacing".

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
This initial trace includes the title screen and cannot diagnose the
background-fight slowdown seen in play testing. A previous attachment attempt against the old build
never connected; the 18:55:31 rendering comparison therefore remained unprofiled.
The runtime selects the AMD RX 7900 XT Vulkan device, so software rendering is
not the explanation. CPU waits, presentation pacing and GPU execution still
need to be distinguished in a capture of the affected scene.

Resolved: see "Backstage brawl frame pacing".

## Background-fight synchronization comparison (19:20)

The scene was confirmed on screen before capture. Screenshot
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

Resolved: see "Backstage brawl frame pacing" (the waits ended at the XMP sleeps).

An optional 1 ms polling-cap patch compiled, but was never linked or tested in
the game. It was deferred after gameplay became smooth in play testing with the
RivaTuner limiter disabled. SDK source and the launcher were restored; the candidate
patch is retained only in `analysis/rexglue-wait-polling-deferred.patch`.
The measured improvement therefore cannot be attributed to that patch.
Do not use `vsync=false` as a presentation-only test: that also changes guest
vblank from the configured video rate to 1,000 Hz in this SDK.

## Smooth gameplay after disabling the external limiter

`analysis/vulkan-play-20260927-191954/` closed normally (603.88 seconds, exit
code zero, no fatal guest targets). In play testing, actual gameplay became very
smooth after turning off the RivaTuner limit. The precise limiter-toggle
time was not recorded. The background screenshot already shows 342 presentation
FPS, so the background's roughly 43 guest FPS must not simply be blamed on the
old 60 FPS limiter; whether that scene still feels slow needs confirmation.
Resolved: see "Backstage brawl frame pacing".

The later 180–600-second interval contains 25,201 frames over 420.0025 seconds:
60.002 guest FPS, median 16.553 ms, p95 18.499 ms, p99 19.012 ms and maximum
23.096 ms. There are no intervals over 33.333 ms. Scene boundaries were not
recorded, but the long steady interval supports the smooth-match report.
Tracy was connected only for the earlier background capture, not this interval.

```powershell
python scripts/analyze_perf.py analysis/vulkan-play-20260927-191954/perf.csv --start 180 --end 600
```

Current working setup: Vulkan on RX 7900 XT, texture compatibility enabled,
RivaTuner frame limiter disabled, original guest timing. Ordinary play does not
need `-Profile`. This limiter interaction was seen in play testing, not proof of the
exact interception/presentation mechanism. Repeated match and cinematic testing
remains necessary before calling the port production-ready.

## Display synchronization and character-select flicker

In play testing, One on One gameplay through the launcher was extremely smooth,
but character select still had a brief black flicker. The latest native-scale
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
it is not a persistent launcher default. Confirmation of screen scope, trigger,
flicker outcome and smoothness in play testing was pending, so this was not
labeled fixed. Resolved: see "Synchronous-shader comparison: unchanged" and
"Resolved: render-target path".

## Character-select visual capture

In play testing the flash persisted. The latest normal launcher run used
async shaders, so this report alone does not confirm the earlier synchronous
comparison outcome. Do not mark that hypothesis ruled out yet.

`scripts/capture-transition.ps1 -WaitForF8 -Seconds 12` arms a window-only capture.
Press F8 immediately before entering character select, then repeat the affected
selection. It saves timestamped JPEG frames and brightness summaries under
`analysis/transition-*`, never stopping the game. Capture samples are best effort:
the initial title-screen check captured 79 frames in two seconds (about 39 FPS),
so a one-frame flash may require repeated transitions. Dark title screens also
score dark; brightness is a locator for visual inspection, not a bug verdict.
Recording overhead makes these runs unsuitable for performance conclusions.

The title-screen capture `transition-20260927-201308-904` verified the recorder.
The 12-second F8-triggered capture is armed for game run
`vulkan-play-20260927-201213-658`; the character-select recording is pending.
No rendering workaround has been promoted.
Resolved: see "Resolved: render-target path".

## Focus-loss freeze: recovery candidate

Play testing reproduced a persistent frozen picture and unusable controls in both
borderless and windowed mode. Run `vulkan-play-20260927-204235-173` remained
responsive to Windows and continued submitting guest frames: the ten-second
frozen-state trace `profile-20260927-204434` contains 447 swap CPU zones.
Two captures of character select were byte-identical, and a Win32 redraw did
not recover the session. Continuing swaps therefore does not establish healthy
gameplay or input.

A brief UI-thread context/stack-memory snapshot located the instruction pointer
in `NtUserMsgWaitForMultipleObjectsEx`, with the immediate return address in
SDL's `WIN_WaitEventTimeout`. This was a stack scan, not a fully unwound debugger
backtrace. It supports an idle UI loop, rather than a blocked Vulkan call at
that instant. Lost/coalesced event wake-ups are a hypothesis, not a proven cause.

The idle-wakeup candidate was rejected. Run `vulkan-play-20260927-205624-662`
initially received an old runtime DLL because the SDK post-build copy did not
run when only the DLL changed. Run `vulkan-play-20260927-210254-094` then used the
verified new runtime and still froze. Its window had no pending paint, so the
50 ms wake-up workaround had nothing to recover. That workaround is removed.

Read-only inspection using compiler-verified object layouts found the presenter
in `kNone` paint mode with `kUnconnectedRetryAtStateChange` and a zero connected
surface size. The window was minimized. Restoring it without changing size left
those states unchanged. Restoring and then changing its width by one pixel
immediately changed the presenter to `kUIThreadOnRequest` and
`kConnectedPaintable`; the original width was restored afterward. This confirms
that a surface-size notification can recover presentation in the same process.
Confirmation of visible output and controls in play testing remains separate
from this internal-state observation.

`patches/rexglue-window-restore.patch` refreshes the presenter surface connection
in `Window::OnRestored`, before listener callbacks. SDL can restore the same
pixel dimensions without triggering `OnActualSizeUpdate`; the presenter still
needs notification after a minimized, zero-area surface was disconnected.
Vulkan builds apply this patch to the pinned SDK through
`scripts/apply-vulkan-patches.ps1`. The changed object files compile; linking,
deployment and repeated restore validation were still to do.

The build script now explicitly synchronizes all three runtime DLLs after each
Vulkan build and verifies their hashes. Matching PDBs are copied for diagnostics.
`tests/test_runtime_sync.ps1` checks stale replacement, unchanged-file preservation
and incomplete-runtime rejection. Each launch records the actual executable and
DLL hashes. Deployment refuses to overwrite this build's running game.
Focus-loss recovery and match smoothness still require runtime validation;
character-select flicker remains open. Resolved for the flicker: see
"Resolved: render-target path".

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

### Character-select capture resumed, 2026-09-27 21:18

The restore patch was linked and deployed.
Build synchronization verified all runtime DLLs (one updated). Restore behavior
still needs runtime confirmation; compilation alone is not acceptance.
Run `vulkan-play-20260927-211756-479` uses native-scale windowed Immediate
presentation, the established invalid-fetch fix, normal asynchronous shaders,
and frame timing capture. Window recording is armed on F8 for 12 seconds in
`transition-20260927-211810-884`. This is the baseline for the flicker comparison;
no visual result is available until the transition is triggered.
### Baseline flicker captured

`transition-20260927-211810-884` contains 720 sampled frames over 12 seconds.
Visual inspection of frames 94, 95, 108 and 194 confirms loss of the background
scene and wrestler previews while roster portraits, labels and buttons remain
visible. This is not a full-window black presentation. Seven intervals start
at frames 95, 138, 190, 236, 283, 330 and 381; durations are 217, 284, 283, 283,
284, 301 and 300 ms respectively (capture timestamps, not GPU frame timings).
The abrupt losses recur while navigating between characters. Cause remains
unproven. Next compare repeated switching between the same two characters in
the same process, then synchronous shaders if necessary. No game restart or
termination is needed for the repeated-character comparison.

### Repeated-character result, 2026-09-27 21:23

Run `vulkan-play-20260927-212323-410`, capture
`transition-20260927-212338-899`: 720 sampled frames. The tester repeatedly switched
between John Cena and John Morrison. Visual checks at frames 41/42, 134 and 578
confirm the same scene loss with the roster UI intact. Ten intervals lasted
300, 300, 282, 282, 299, 282, 316, 284, 301 and 268 ms; exact endpoints are in
`black-intervals.json`. Repeated returns do not eliminate the issue. This weakens
the first-use shader/resource warm-up explanation, but does not rule out an
asynchronous rendering or resource-lifetime fault. Next controlled comparison:
keep settings identical and add `-SynchronousShaders`, capture the same two
characters.

### Synchronous-shader comparison: unchanged

Run `vulkan-play-20260927-212533-128` confirms
`--async_shader_compilation=false` in the recorded launch arguments. Capture
`transition-20260927-212546-821` has 720 sampled frames. Eleven complete black
scene intervals lasted 282–301 ms, plus one interval cut off by recording end.
Frame 52 visibly retains the roster/UI while losing the background and preview.
The tester saw the same symptom. Disabling async compilation does not fix it;
normal launch defaults are unchanged.

The next diagnostic is draw submission, rather than another rendering option.
`patches/rexglue-flicker-diagnostics.patch` (since removed) adds the opt-in, default-off
`vulkan_draw_diagnostics` flag. Each `[DEBUG-flicker]` swap summary records source
address/size, copy-mode request count, and per active vertex/pixel shader pair:
draw requests / host draws recorded / requested indices / recorded vertices.
Counts are collected on the GPU command thread. Recorded host draws are not proof
of correct final pixels. Normalized pixel shader usage may differ from the active
guest shader, so these pairs identify command groups rather than final pipelines.
Compare group disappearance against drops inside the backend before examining
render targets/textures. This temporary diagnostic is not for performance scoring
and must be removed after investigation. The modified C++ object compiles.

`scripts/play-vulkan.ps1 -DrawDiagnostics` enables this explicitly. The window
recorder now includes Unix UTC milliseconds to align visual events with log times.
Upstream compatibility issue
https://github.com/xenia-project/game-compatibility/issues/1505 was consulted but
its visible report does not diagnose this character-select symptom.

### Draw diagnostic result, 2026-09-27 21:32

Run `vulkan-play-20260927-213116-812`, capture
`transition-20260927-213134-333`, analysis `compare-flicker-draws.py` and
`draw-comparison.json` in the capture directory. The recorder captured twelve
complete flashes. UTC capture timestamps were compared with local-time log
entries converted to epoch milliseconds; a 65 ms exclusion around each visual
transition reduces capture/presentation boundary ambiguity. The retained sample
contains 89 black-scene and 308 visible-scene swaps.

All retained samples have four copy-mode requests per swap and the same reported
frontbuffer source/size. Median requested/submitted draws are 323/323 during
black intervals and 322/322 during visible intervals. Requested and recorded
draw counts match for every shader group in this sample. Multiple shader groups
with substantial geometry disappear from requests during black intervals, while
the persistent menu groups continue. For example, all four pixel shader groups
paired with vertex shader `01B64B30579F80F6` have zero requests during black
samples. This rules against Vulkan IssueDraw early-return drops explaining
those missing groups; it does NOT prove that final render-target contents are
correct or that the guest intentionally blanks the scene.

Two earlier filtering paths exist in the common command processor: type-3
packet bin predicates, and visibility-query kill draws. The temporary diagnostic
now logs `[DEBUG-flicker-packet]` for each of these skipped cases, including
opcode/bin selection/mask for predicates and index count for visibility skips.
The diagnostic flag is defined in the common processor and declared by Vulkan.
Both modified objects compile. The next run can distinguish these filters from absent
guest commands without changing rendering behavior or introducing a workaround.

### Packet filters excluded; loading-state probe prepared

Run `vulkan-play-20260927-213646-021`, capture
`transition-20260927-213701-259`: 97 black-scene and 296 visible-scene swaps after
boundary exclusions reproduce the earlier missing-group pattern. No
`[DEBUG-flicker-packet]` events appear in the log covering the capture. Preserved
the first rotated log as `runtime-preserved.log` in the capture directory so
continuous diagnostic logging cannot erase that evidence. Common bin-predicate
and visibility-kill filters do not explain this captured loss. This is not proof
that all GPU/texture behavior is correct.

Read-only analysis of the existing guest image found registration-table pairs:
`StartNowLoading` -> 0x8274B868, `EndNowLoading` -> 0x8274B8A0,
`IsNowLoadingFade` -> 0x8274B8D8. Generated code maps these to a manager pointer
at guest 0x82EC1AA0. The query sub_825849B8 checks manager+60 and delegates to
sub_82595860, which returns true when child+36 is zero and child+16 is nonzero.
The Lua-facing IsNowLoadingFade wrapper inverts that result; the sampler records
the inner query value, not a confidently named fade state.

In the session under test the logged guest virtual base was 0x200000000; the
manager pointed to 0xAED2B8D0 and the child to 0xABEBAE20.
`analysis/capture-loading-state.py` opens the process read-only, waits for F8,
and samples manager/child fields for 13 seconds with UTC timestamps. It never
writes guest memory or suspends the process. It is session-specific diagnostic
code, not a supported launcher feature. Next compare state changes to a fresh
12-second visual capture in the same session; no rebuild or restart is required.

### Loading-fade probe result: negative

Capture `transition-20260927-214223-296` contains 720 visual samples and twelve
black-scene intervals lasting 282–299 ms. The companion read-only probe
`loading-state-213646.csv` contains 2,393 samples; its start is 10 ms before the
visual recording. Analysis `compare-loading-state.py` writes
`loading-comparison.json` beside the visual capture.

With 65 ms excluded around transition boundaries, all 349 black-scene samples
and 1,283 visible-scene samples return inner query sub_825849B8 = 1. Every sampled
32-bit field in the manager's first 64 bytes and child's first 40 bytes is
identical between those groups. The Lua wrapper's IsNowLoadingFade result is
therefore false throughout these samples. This particular global loading/fade
controller does not correlate with the flicker; do not patch or bypass it.
This does not exclude other loading or character-preview controllers. No guest
memory was changed. No more identical captures are requested at this point;
further work needs a more specific character-preview/render-generation seam.

### Resolved: render-target path, 2026-09-27 22:55

Cause: the default Vulkan host-render-target (FBO) path does not preserve the
scene across frames in which the guest skips its 3D pass. On each switch the game
stops issuing the stage/preview draws for 17–18 frames while it swaps models, but
keeps its four resolve copies per swap. Real hardware keeps re-resolving the last
scene still held in EDRAM (the tester saw no flash on console); the FBO path
resolves black. Fragment shader interlock (`--render_target_path_vulkan=fsi`),
which emulates EDRAM directly, keeps the stage visible; only the preview slot is
empty while the new model loads, as expected.

Evidence, all at native-scale windowed Immediate presentation:

| Path | Switches | Flashes |
| --- | --- | --- |
| FBO (previous default) | 20 over runs `…222814-220`, `…224128-408` | 20, each 17–18 frames (282–301 ms) |
| FSI | 26 in run `…224548-178` | 0 |
| FSI via new default, clean build, run `…225649-268` | 10 | 0 |

Performance, run `vulkan-play-20260927-224923-769` (FSI, Cena vs Orton, Raw arena,
warm cache): gameplay 262–308 s after first swap has 59.98 guest FPS, median
16.63 ms, p95 19.05 ms, p99 20.04 ms, max 31.64 ms, no intervals over two frame
budgets. The FBO baseline above is 59.99 / 16.53 / 18.04 / 18.54 / 33.12 ms. Menus
run at 42–46 FPS as they did before. One short match only; gameplay p95/p99
are about 1–1.5 ms higher, so a longer comparison is still worthwhile.

Resolved: `patches/rexglue-xmp-no-delay.patch` fixed the menu rate. Session
`vulkan-play-20260928-075634-047` held 60.00 FPS from the title screen onward
(10-second windows), including menus and backstage. See "Backstage brawl frame
pacing".

Ruled out along the way: a single long guest wait (a kernel wait tracer showed no
wait spanning a flash), CPU-bound loading (per-thread CPU during a switch matched
idle), file I/O (synchronous and immediate). The 22 ms loader cadence observed in
one session is the game's 10 ms job scheduler alternating two worker groups; it
is unchanged under FSI.

`scripts/play-vulkan.ps1` now passes `--render_target_path_vulkan=fsi` by default
(`-RenderTargetPath fbo` opts out; the runtime falls back to FBO automatically on
GPUs without fragment shader interlock). The launcher uses the same script. All
temporary `[DEBUG-flicker*]` instrumentation and its patches/switches were removed.

Regression check: launch with `scripts/play-vulkan.ps1 -KeyboardInput` (enables
`mnk_mode`; keyboard emulation is off by default and ignores keys until the window
receives a focus change), reach One on One character select, then run
`scripts/check-charselect-flicker.ps1`. It briefly takes focus, alternates D/A,
and exits 1 if any scene-dark run of three or more frames appears.

## Rare single hitches after the FSI switch, 2026-09-27 23:10

The tester noticed a very brief freeze during a match in launcher run
`vulkan-play-20260927-230254-917` (FSI, no perf capture). The log shows new
graphics pipelines created mid-match (23:04:06–07, 23:04:13, 23:05:04) and the
one-time warning "Skipping Vulkan frame presentation due to async placeholder draw
usage" at 23:04:06.372. FSI keeps its own pipeline storage
(`cache/vulkan/shaders/shareable/5451085D.fsi.vk.xpso`, 9.5 KB vs 16.9 KB for FBO),
so moves and effects already warmed under FBO compile once more under FSI. Each
distinct pipeline costs this once, then loads from storage at boot.

Cost measured in `vulkan-play-20260927-224923-769` (FSI, perf capture): gameplay
262–308 s had max 31.6 ms, but the entrance/match-start burst (225–258 s) had 29
guest frame intervals of 49–1018 ms. The guest itself is blocked, not only the
present: for every new pipeline the GPU thread compiles a placeholder pipeline
synchronously (40–70 ms apart in the log) before queueing the real one. That
placeholder is never bound, because draws are skipped while a pipeline is a
placeholder. Pipelines without a guest pixel shader never take the async path at
all; under FSI their generated fragment shader carries the EDRAM depth logic.
The `pipeline_cache_misses` perf column is never populated.

Experimental change `patches/rexglue-async-pipelines.patch`, off by default via
`--vulkan_async_skip_placeholder_pipelines` (`play-vulkan.ps1
-SkipPlaceholderPipelines`): pending pipelines skip their draws without a
placeholder compile, and vertex-only pipelines are deferred too unless their
vertex shader memexports. A failed background creation now clears the pending
state instead of skipping forever. It shipped in commit 74979b1 as an
off-by-default switch; the validation described below is still to do. Expected effect: long guest stalls on first-seen pipelines
drop to shader translation time; presentation is still held while real pipelines
compile, which background threads do in parallel.

Validation still to do before promoting it: an FSI run with perf capture and
cold pipelines, with and without the switch, compared over the entrance/match-start
window (count of >50 ms intervals, max), plus a visual check that shadows and
effects appear normally once compiled. Without the switch, hitches should still
fade as the FSI storage fills.

## Backstage brawl frame pacing (2026-09-28)

Backstage brawls ran at 43–46 guest FPS while matches held 60. The frame intervals were
bimodal, ~16.7 ms and ~33 ms (`analysis/vulkan-play-20260928-073701-481/perf.csv`).
That is a 60 FPS loop missing vblank, not a 30 FPS target. An RPCS3 forum thread for
the PS3 version (https://forums.rpcs3.net/archive/index.php/thread-204858.html)
reports 60 FPS backstage. The FBO render-target path showed the same pattern
(`vulkan-play-20260928-073957-347`), so FSI was not the cause. A 1 ms
`timeBeginPeriod` request changed nothing: guest `Sleep(1)` already averaged 1.08 ms.

No thread was CPU-bound; the busiest used under 10% of a core. The Tracy timeline
(`profile-20260928-074549`) showed the frame gated by a chain of waits. A worker
thread called `XMsgInProcessCall` twice per frame at ~10.6 ms each. The main thread's
sleeps and the GPU thread's once-per-frame `WAIT_REG_MEM` on guest memory `0x1353D004`
ended at the same instants. The call is `XMPGetPlaybackController`. The SDK's XMP app
sleeps 10 ms whenever it is called off the main thread, a workaround for another title
that polls it in a tight loop; `XMPGetStatus` similarly sleeps 1 ms.
`patches/rexglue-xmp-no-delay.patch` replaces both sleeps with a yield. With it, the
GPU thread completes 300 frame handshakes per 5 s (60 FPS) backstage, and play was
smooth in testing (`vulkan-play-20260928-075634-047`).

`play-vulkan.ps1 -ExtraArguments '--cvar=value'` passes additional runtime flags for
experiments like these.

## Frame pacing at 60 FPS (2026-09-28)

A RivaTuner 60 FPS cap stuttered, and play without it was not perfectly even.
Guest frames averaged exactly 16.666 ms but individual swaps landed 13–21 ms
apart (1.45 ms standard deviation, `vulkan-play-20260928-075634-047`). The
runtime presented each swap immediately, so that jitter reached the screen; the
console only flips at vblank. A second 60 FPS clock (RivaTuner) then decided per
frame whether to show it now or a whole frame later.

`patches/rexglue-frame-pacing.patch` makes three changes:

- The guest vblank clock sleeps on a high-resolution waitable timer instead of
  polling with `Sleep(1)`. An earlier version added a short spin on this thread;
  menus and matches then fell to 30–45 FPS (`vulkan-play-20260928-094258-241`).
  This thread must not spin.
- A dedicated presentation thread holds each guest frame and presents it on the
  predicted ideal vblank time, waking 1 ms early and spinning the rest. Guest
  frames arrive 0.7–1.5 ms after each tick, so presenting 1 ms after the tick
  collided with them (50–57 FPS, `vulkan-play-20260928-100255-396`); presenting
  on the tick does not. `--present_pace_to_guest_vblank=false` restores
  immediate presentation, and `--present_pacing_stats=true` logs intervals,
  empty vblanks, replaced frames and host presents every 5 seconds.
- The achievement toast overlay is registered only while a toast is queued. Any
  registered overlay made the window thread repaint at the monitor refresh rate
  (360 Hz on the test PC) after the first input event, which also explained the
  342 FPS RivaTuner reading.

With VSync on the 360 Hz test display (`vulkan-play-20260928-100512-522`): 301
presents per 5 seconds, 0.10–0.15 ms interval standard deviation, no empty
vblanks and no replaced frames. The tester reported it as extremely smooth
without a limiter. Entrances render at 30 FPS in the game itself and are paced
evenly at that rate. The launchers now default to VSync.
