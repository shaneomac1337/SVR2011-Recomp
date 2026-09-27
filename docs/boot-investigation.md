# First native boot investigation

Scope authorized on 2026-09-27: begin an SVR 2011 native Windows recompilation
using the existing Xbox 360 ISO. The first deliverable is a reproducible extraction,
code-generation and build path, plus evidence from a bounded startup run.
A title screen, playable match and complete port are later milestones.

## Acceptance checks

- Keep the input ISO unchanged and exclude original game data from Git.
- Identify the exact executable revision and reject a mismatched build input.
- Inventory and extract the disc with a repeatable command.
- Pin tools and verify official download checksums.
- Generate code without disabling the analysis validation gate.
- Compile and link a Windows executable.
- Run a bounded startup probe, retain logs and report failures honestly.
- Run background command-line tools without visible command-prompt windows.

## Established baseline

- XEX SHA-256: `6aead4cb29caa11f245d37b36277b5ab57772cffa2cf8c459d3295a3b2d3ebc1`.
- Title `5451085D`, media `4A4F538E`; image base `0x82000000`, entry `0x8215B140`.
- 366 disc files inventoried and extracted. Local file inventory and hashes:
  `analysis/disc.json`.
- ReXGlue v0.10.0, source revision `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`.
- LLVM 21.1.8; existing VS 18 BuildTools and Windows SDK 10.0.26100.0.
- RelWithDebInfo build compiled and linked successfully (206 build steps).

## Function discovery

Initial strict analysis failed on four direct branch targets. An exploratory
forced generation was used only to inspect surrounding code. The manifest now
seeds those addresses; a subsequent **strict** generation passed. No fabricated
return values or no-op replacement functions were used.

| Address | Evidence from generated instructions |
| --- | --- |
| `0x8227FD28` | Load through `r3`, tail-branch to `0x823020A0` |
| `0x824C6E38` | Dispatch based on object field at offset 1100 |
| `0x82BBA778` | Adjust `r3` by 8, tail-branch to `0x82BBB678` |
| `0x82C2A020` | Copy two words to object offsets 32 and 36, return |

First native run initialized the graphics/audio runtime and entered guest code,
then failed on an unregistered indirect target `0x8216E160` (exit `0xC0000409`).
That target was added as a discovery seed. This is runtime-guided function
discovery; clean static analysis alone cannot prove every indirect target exists.

The runtime also logged physical allocation failures during initialization.
Their causal relationship to startup failure has not been established.

## Latest startup result

Runtime-guided discovery subsequently added `0x826E0F68` and `0x826E4D40`.
After strict regeneration and a successful incremental build, the run recorded in
`analysis/runtime-20260927-175542/` survived the full 20-second probe. The harness
terminated it at the deadline (`timed_out: true`, process exit `-1`). There was no
fatal unregistered-function message in this run. Logs show D3D12 initialization,
shader-cache loading, and continuing audio/playback calls.

This proves progress beyond the observed early crashes, not a responsive game:
a menu, rendered frame, controller response, and a complete match have **not**
been visually verified. A stalled process can also survive a timeout.

Code review found a report-path collision that could replace an extracted file
with JSON. The extractor now rejects those destinations before reading/extracting;
two regression tests preserve the original bytes. All nine Python tests pass.
The Spec review reported no material findings for this initial investigation.

## Remaining work

Track subsequent runtime results here. Do not describe a successful compile or
a process surviving the smoke timeout as proof of a playable game. Menu and match
milestones require separate visual and interaction verification. Next, inspect a
bounded graphical run and determine whether the game is rendering, waiting for
input, or stalled during initialization. Repeated physical allocation errors and
unknown audio-register writes remain observations, not established root causes.

## WWE-logo crash investigation

The user reported a crash while pressing Start at the WWE logo. Their log
(`analysis/wwe-logo-user.log`, preserved from `svr2011_002.log`) ended with an
unregistered target `0x82CFD9C8`. A longer startup probe reproduced that exact
fatal address in `analysis/runtime-20260927-180039/`.

Adding that entry revealed a second missed target, `0x82CF7068`, in
`analysis/runtime-20260927-180218/`. The user had not interacted with that test,
so Start is not a necessary trigger: the timed intro transition also reaches it.
Generated instructions show valid dispatch thunks at both addresses: the first
calls through an object vtable, and the second adjusts `r3` by -4 then tail-calls
`0x82CF7110`. Neither was replaced by a stub. Strict code generation and native
build succeeded after registering both entry points.

The resulting build visibly reached the full SVR 2011 title screen with John Cena
and "Press START button" (captured with PrintWindow from the game's own window).
This is the first visually verified title-screen milestone. The next check is
pressing Start there and observing whether a menu appears.

The smoke harness now defaults to info-level logs, records elapsed runtime and
fatal guest addresses, and returns an error for a crashed process. Timeout still
means only that the process was alive at the deadline, not a gameplay pass.

## Menu and gameplay rendering

The user confirmed reaching and controlling the menu. Subsequent background
gameplay execution reached missing callbacks `0x82ACF950` and `0x82ACF9A0`;
both were registered, regenerated with strict validation, and rebuilt. The former
is an argument-adjusting virtual-call thunk; these are genuine function entries,
not success-returning replacements.

The next hardware run (`analysis/runtime-20260927-180744/`) terminated after
31.23 seconds with D3D12 `0x887A0006`, `DEVICE_HUNG`. The user independently
reported a driver crash. Adapter: AMD Radeon RX 7900 XT, Windows driver version
32.0.31041.3013. No further hardware-rendered runs were started after that report.
Invalid texture-fetch descriptors appeared before the hang, but causality is
unproven; the compatibility bypass was not enabled speculatively.

The smoke harness now defaults to WARP software rendering. The first diagnostic
run (`analysis/runtime-20260927-180957/`) confirmed Microsoft Basic Render Driver
(vendor 0x1414/device 0x008C), enabled DRED, and survived its 60-second deadline
with no fatal guest target or device-removal message. A captured frame showed
"Load Successful" over the title screen. The D3D12 debug layer is unavailable on
this machine; requesting it logs a warning, while DRED still initializes.

This is an alternate diagnostic path, not a fix for the hardware hang. Verify the
same background-gameplay scene in software before comparing renderer behavior.

The follow-up software run (`analysis/runtime-20260927-181139/`) was stopped by
the agent after the user reported a black screen and unacceptable performance.
This was a deliberate external termination, not another observed game crash.
WARP is not a usable gameplay workaround, and the hardware renderer defect remains
unresolved. All test instances were closed. The original title/menu progress is
preserved; no additional hardware runs or driver-setting changes were made.

Next investigation: collect validated D3D12 draw/shader diagnostics for the
background-gameplay scene, or prepare a separate Vulkan-enabled SDK build for
comparison. The installed Windows SDK package has `REXGLUE_USE_VULKAN=OFF`, so
Vulkan cannot be selected merely by changing the launch arguments. Do not treat
the invalid-texture warning, SDK compatibility flags, or a backend switch as a
proven fix without a controlled test.

## Vulkan comparison build (2026-09-27)

The user authorized proceeding with Vulkan. Built the pinned v0.10.0 SDK source
(`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`) together with the host in
`out/build/win-amd64-relwithdebinfo-vulkan/`, with Vulkan ON and D3D12 OFF.
The original D3D12 executable and DLLs were preserved. Vulkan uses separate
cache and userdata subdirectories. No drivers or machine-wide SDKs were installed.

Source-build integration needed the SDK's x86-64-v2 compiler flags, an explicit
native Python executable (CMake initially selected a shim), materialization of
libmspack's Windows symlink stubs, and an ImGui include directory for host UI
headers. These are captured in the scripts and root CMake file. The SDK's version
label says `0.10.0.0-dev.unknown` when used as a subdirectory because its version
helper inspects the parent repository; the source commit is separately verified.

The startup test in `analysis/runtime-20260927-182712/` ran for 25.18 seconds and
was deliberately terminated at its 25-second deadline (`alive_at_deadline`).
The log confirms **Vulkan device AMD Radeon RX 7900 XT**, API 1.4.349, vendor
0x1002/device 0x744C. `analysis/vulkan-startup.png` shows the WWE safety intro
rendering, with a Vulkan overlay reading 60 FPS. This is an intro observation,
not a gameplay benchmark. No fatal guest targets, `VK_ERROR`, `DEVICE_LOST`, or
`DEVICE_HUNG` messages appeared in this run. Heap-allocation errors and missing
sound-bank warnings remain in the log; startup survival does not resolve those.

The regular `scripts/play-vulkan.ps1` launcher was then opened for user testing.
In `analysis/vulkan-play-20260927-182805/`, a later captured frame
(`analysis/vulkan-play.png`, around 18:29:25) shows Cena and Orton rendered in the
ring with the "Press START button For Main Menu" prompt. The overlay reads
Vulkan / 60 FPS. The process remained responsive and no fatal guest-function or
device-loss message had appeared. This verifies rendering beyond the intro;
controller behavior and sustained match stability still need user confirmation.
The launcher has
no diagnostic timeout; close the game window to stop. Runtime logs are written
to `analysis/vulkan-play-<timestamp>/`.

Validation: successful configure/build, PowerShell syntax checks, all nine
existing unit tests, repeatable source preparation, Vulkan/WARP rejection before
launch, and independent Standards/Spec reviews with zero blocking findings.

## First match-start failures and measured slowdown

The user confirmed playing the background fight, then reported a crash starting
One on One, Randy Orton versus Rey Mysterio (arena unspecified). The regular
Vulkan run above ended at 18:30:01 with missing guest target `0x82BFAE18`.
The original image contains `4E800020` (`blr`) there, followed by zero padding,
with four read-only references at `820B07DC`, `820B0878`, `820B08C0`, and
`820B2A60`. Added the true function entry to the manifest, regenerated strictly,
and verified the emitted registration and original return behavior. This is not
a fabricated success stub or a GPU device-loss error.

The retest, `analysis/vulkan-play-20260927-183339/`, reached the match according
to the user, then exited at 18:35:20 on missing target `0x82ACF9B8`. The agent did
not terminate this run. That address is a six-instruction virtual dispatch thunk
ending in `bctr`, adjacent to the previously registered `0x82ACF9A0`. It was also
added as a discovery seed. The next run, `analysis/vulkan-play-20260927-183832/`,
remained running beyond both prior crash points. A frame captured around 18:40:05
(`analysis/vulkan-match-retest.png`) shows Orton and Mysterio fighting in the
selected match with a crowd and wrestler HUD. That run then crashed at 18:40:12
on missing callback `0x82600EB8`; it was not terminated by the agent. The user
confirmed the match was working before exit and that background-fight performance
felt better. The third target is another six-instruction virtual dispatch thunk,
referenced at `0x82035ED0`; it has been seeded and its generated implementation
checked against the original instructions. Runtime validation remains pending.

Added optional `-PerfCapture` to both launchers. The app's `OnPostSetup` opens
the SDK's existing `perf_log_csv` output before guest execution; v0.10.0 declares
the option and has the CSV writer but does not initialize the file itself.
The graphics plugin records intervals at `PM4_XE_SWAP`. Binary inspection in
review confirmed the host and plugin use the same runtime counter state. The SDK
locks the CSV on Windows, so read it after exit. A forced crash/timeout can lose
the last buffered frames.

The retest produced 3,780 CSV rows spanning 99.19 seconds of positive guest-swap
intervals. Three consecutive ten-second windows averaged 32.41, 32.20, and
32.10 guest FPS (median frame time ~33.06 ms; 95th percentile ~45.9 ms). Another
window included an 816 ms stall. These measurements record game output rather
than the external overlay's apparent 60 FPS. The expected guest-swap rate and
simulation speed for this scene have not been established, so the relationship
between the ~32 FPS windows and perceived slow motion remains unverified.
Shader compilation and invalid-texture warnings are present, but causality
has not been demonstrated. No timing, vsync, or invalid-texture bypass settings
were changed.

The later CSV contains 4,500 rows over 98.54 seconds: middle ten-second windows
averaged roughly 39.7–42.6 guest FPS and the final partial window averaged 59.9.
This is consistent with the user's improvement report, but scene and cache state
were not controlled, so it does not establish a performance fix.

User preference: do not stop interactive tests. Use only the no-timeout
`play-vulkan.ps1` for user sessions; do not apply the bounded smoke harness to a
session they are playing. The launcher now waits for process exit and records
`result.json` plus normal-exit/crash output without killing the process. After
the third callback rebuild, leave the next launch to the user. All three match
exits above have fatal guest targets in their logs, rather than agent timeouts.
