# SVR 2011 recompilation investigation

Experimental Xbox 360 → Windows recompilation using ReXGlue. **Gameplay is not yet reliable.**
The title screen renders and the user confirmed menu/gamepad interaction. Background
gameplay exposed further missing callbacks, followed by a D3D12 device hang on an
AMD RX 7900 XT. Hardware-rendered testing is paused; the diagnostic harness now
defaults to WARP software rendering.
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

## Dependencies

- [ReXGlue v0.10.0](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0)
- [LLVM 21.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8)
- Microsoft C++ headers/libraries, Windows SDK, CMake and Ninja from BuildTools

The application scaffold and `generated/rexglue.cmake` originate from ReXGlue's
project templates. See `third_party/rexglue-LICENSE.txt` for their license.
