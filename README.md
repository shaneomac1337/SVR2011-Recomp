# SVR 2011 recompilation investigation

Experimental Xbox 360 → Windows recompilation using ReXGlue. **Gameplay is not yet reliable.**
The title screen renders and the user confirmed menu/gamepad interaction. Background
gameplay exposed further missing callbacks, followed by a D3D12 device hang on an
AMD RX 7900 XT. A separate Vulkan-only source build is available for comparison;
the original D3D12 diagnostic harness still defaults to WARP software rendering.
See [the boot investigation](docs/boot-investigation.md) for measured results.

This project uses locally supplied game data. Original disc images, extracted
files, generated game translations, tools and build outputs are excluded from Git.

## Reproduce on this Windows machine

Use PowerShell 7 and Python 3. The build script defaults to the existing VS 18
BuildTools and Windows SDK paths on this machine; pass `-VisualStudioRoot` and
`-WindowsSdkRoot` for other installations. LLVM and ReXGlue are extracted into
`.tools/`, without running a system installer.

```powershell
./scripts/bootstrap.ps1
python scripts/inspect_disc.py 'WWE SmackDown vs. Raw 2011 (USA, Europe).iso' --extract-all assets
./scripts/build.ps1 -Action Codegen
./scripts/build.ps1 -Action Configure
./scripts/build.ps1 -Action Build
./scripts/smoke.ps1 -Seconds 60 -GpuDiagnostics
```

`bootstrap.ps1` needs 7-Zip to unpack LLVM (default `D:/tools/7-Zip/7z.exe`;
override with `-SevenZipPath`). Downloads are pinned and SHA-256 checked against
the release asset digests. The original ISO is read only. Repeated extraction
verifies existing files and refuses to overwrite differing contents.

The build accepts only the recorded XEX fingerprint. Supporting another revision
requires fresh analysis rather than reusing addresses blindly.

`smoke.ps1` records its exit status in a timestamped directory under `analysis/`.
It defaults to `-Adapter Warp` (`--d3d12_adapter=-2`), which uses Microsoft's CPU
renderer. This is a diagnostic fallback, not a hardware-renderer fix. The earlier
software test also produced poor performance and a user-reported black screen;
it is not a usable gameplay workaround. The earlier
direct launch command selects hardware and can reproduce the driver hang.
`-Adapter Hardware` remains available for deliberate future investigation, but
has not been re-tested since the device hang. `-GpuDiagnostics` requests D3D12
debugging and DRED; the debug layer is currently absent on this machine, while
DRED initializes successfully.
It forcibly stops its own process at the deadline. That is a diagnostic timeout,
not a successful gameplay test. The SDK's `--headless` suppresses prompts; a
graphics window may still appear. Command-line child processes use hidden launch.

```powershell
python -m unittest discover -s tests -v
```

## Vulkan build and launch

The Vulkan build uses the same ReXGlue v0.10.0 commit and generated game code,
with `REXGLUE_USE_VULKAN=ON` and `REXGLUE_USE_D3D12=OFF`. Its executable and
DLLs live in `out/build/win-amd64-relwithdebinfo-vulkan/`. The existing D3D12
build is preserved. No system Vulkan SDK or driver installation is needed.

After the initial bootstrap, extraction and code generation above:

```powershell
./scripts/prepare-vulkan.ps1
./scripts/build.ps1 -Renderer Vulkan -Action Configure
./scripts/build.ps1 -Renderer Vulkan -Action Build -Jobs 6
./scripts/play-vulkan.ps1
```

Use `./scripts/play-vulkan.ps1 -PerfCapture` to record guest frame intervals and
runtime counters to `perf.csv` alongside the run log. Close the game before
reading the CSV: the SDK holds the file exclusively on Windows. Frame intervals
measure guest swap submissions, unlike an external presentation-FPS overlay.
The optional capture does not change game timing or renderer settings.

The [performance investigation](docs/performance.md) records baseline timings,
the user-validated texture compatibility setting (now enabled by default),
profiling commands, and acceptance criteria for the One on One path.
Use `-Experiment Baseline` to compare without that texture setting, or `-Profile`
to enable an on-demand Tracy connection. Smooth gameplay still needs validation.

The launcher opens the game without an additional console window and records
logs under `analysis/vulkan-play-<timestamp>/`. Close the game window to stop.
The PowerShell launcher waits without a timeout, then reports normal exit or a
crash and saves `result.json`. It never terminates the game. Use this launcher for
interactive testing; `smoke.ps1` intentionally has a deadline and is unsuitable
for an open-ended play session.
Vulkan uses separate `cache/vulkan/` and `userdata/vulkan/` directories, so its
first launch has fresh settings and saves. It selects a Vulkan GPU automatically;
check the runtime log for the selected adapter. There is no D3D12 fallback in
this build. Rendering remains experimental; reaching a menu does not establish
gameplay stability.

For a bounded diagnostic run that terminates its own process after 60 seconds:

```powershell
./scripts/smoke.ps1 -Renderer Vulkan -Seconds 60
```

`-Adapter Warp` is rejected for Vulkan. `-GpuDiagnostics` requests Vulkan
validation (requires separately available validation layers); the normal test
does not require those layers. `prepare-vulkan.ps1` materializes libmspack's
source symlink stubs on Windows and refuses to overwrite edited source files.

## Dependencies

- [ReXGlue v0.10.0](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0)
- [LLVM 21.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8)
- Microsoft C++ headers/libraries, Windows SDK, CMake and Ninja from BuildTools

The application scaffold and `generated/rexglue.cmake` originate from ReXGlue's
project templates. See `third_party/rexglue-LICENSE.txt` for their license.
