# SVR 2011 recompilation investigation

Experimental Xbox 360 → Windows recompilation using ReXGlue. **Not yet playable.**
The native executable builds and survives a 20-second startup probe after fixing
observed missing function entries. Menu rendering and gameplay remain unverified.
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
./scripts/smoke.ps1 -Seconds 20
```

`bootstrap.ps1` needs 7-Zip to unpack LLVM (default `D:/tools/7-Zip/7z.exe`;
override with `-SevenZipPath`). Downloads are pinned and SHA-256 checked against
the release asset digests. The original ISO is read only. Repeated extraction
verifies existing files and refuses to overwrite differing contents.

The build accepts only the recorded XEX fingerprint. Supporting another revision
requires fresh analysis rather than reusing addresses blindly.

`smoke.ps1` records its exit status in a timestamped directory under `analysis/`.
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
