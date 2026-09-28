# SVR 2011 PC recompilation: feasibility

Status (2026-09-28): this is a dated historical assessment. The port has since been
built and plays One on One matches on Vulkan; see `boot-investigation.md` and
`performance.md`.

Research date: 2026-09-27. This is an initial investigation, not a completed port or a successful build.

## Assessment

A native PC recompilation is a credible engineering project. The best-supported starting hypothesis is the Xbox 360 executable plus ReXGlue, following the public SVR 2007 project. Success for 2007 is evidence that this approach can work for a related game, not proof that 2011 will work unchanged. Recompilation produces translated code; recovering the original developer source is a separate, much larger objective.

ReXGlue translates Xbox 360 PowerPC instructions to C++ ahead of time and supplies a runtime derived from Xenia. Native CPU code still needs console services such as graphics, audio, filesystem and threading support. Its documentation explicitly describes manual function-boundary analysis, missing imports and code-generation iteration; it is a development toolkit rather than a one-click converter. [SDK](https://github.com/rexglue/rexglue-sdk), [architecture and scope](https://github.com/rexglue/rexglue-sdk/wiki)

## Verified local starting point

Inspection of the retail ISO found:

| Property | Observation |
| --- | --- |
| Local file | `WWE SmackDown vs. Raw 2011 (USA, Europe).iso` |
| Image size | 7,838,695,424 bytes |
| XDVDFS partition base | `0xFD90000` (volume marker at base + `0x10000`) |
| Root executable | `default.xex`, 15,020,032 bytes, root sector 415072 |
| Executable magic | `XEX2` |
| Title ID | `5451085D` |
| Media ID | `4A4F538E` |
| Version | `00000001` |
| Executable SHA-256 | `6aead4cb29caa11f245d37b36277b5ab57772cffa2cf8c459d3295a3b2d3ebc1` |

These observations establish an Xbox 360 input suitable for further inspection. They do not establish dump completeness, successful execution, or toolchain compatibility. No game data was downloaded for this research.

## Existing work and its limitations

**SVR 2007:** HollywoodAkeem's public repository describes a Windows static recompilation. The maintainer reports working menus, match modes, season mode, create-a-superstar, save/load and XInput at retail timing. It also lists skinned-model artifacts, restricted internal resolution/aspect ratio, remaining indirect-call fallbacks and intermittent D3D12 device-loss crashes. These are maintainer claims inspected in documentation, not gameplay independently tested here. [Project and known issues](https://github.com/HollywoodAkeem/SVR07-Recomp)

The releases page provides a v1.0 release and earlier milestones documenting progression through loading, controller assignment, character selection, matches, timing and saves. Its author also maintains a ReXGlue fork described as containing Yuke's-specific patches. This fork merits comparison with upstream before selecting a dependency revision. [Releases](https://github.com/HollywoodAkeem/SVR07-Recomp/releases), [Yuke's fork](https://github.com/HollywoodAkeem/rexglue-sdk-yukes)

**SVR 2011:** A June 2, 2026 commenter in the SVR 2007 announcement claims to be developing a 2011 recompilation with improved rendering and Linux support. This is only a lead: no public source repository or reproducible release was verified in this investigation. Web searches and GitHub repository searches for `svr2011`, `svr recomp`, `smackdown recomp`, and `rexglue 2011` did not locate one. This does not prove none exists. [First-person claim in announcement thread](https://www.reddit.com/r/emulation/comments/1toqr07/i_made_a_static_recompilation_of_smackdown_vs_raw/)

**Toolchain:** GitHub's latest-release API returned ReXGlue `v0.10.0`, published August 21, 2026. The wiki home still refers to `v0.3.x`; that section should not be treated as the current release version. Pin and record the selected revision before development. [Release](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0), [release API](https://api.github.com/repos/rexglue/rexglue-sdk/releases/latest)

The getting-started guide specifies CMake 3.25+, Ninja and, on Windows, Visual Studio 2022 C++ development components with Clang 20+ and LLVM MSBuild support. Tool availability on the test PC (Windows 11, RX 7900 XT) had not been established by this research. [Build prerequisites](https://github.com/rexglue/rexglue-sdk/wiki/Getting-Started)

## Proposed milestones and acceptance checks

1. **Reproducible baseline:** retain the executable hash, inspect its imports and sections, inventory the available build tools, pin the SDK and document extraction from the local input. Keep original game files outside publishable source.
2. **First native boot:** generate and compile the executable translation; capture runtime logs and resolve the first failing import, function dispatch or initialization path. Acceptance: execution reaches game initialization without an immediate crash.
3. **Interactive menu:** resolve graphics, audio, timing and controller problems. Acceptance: visible main menu with responsive input and stable navigation.
4. **One complete match:** select two wrestlers and an arena, load a match, finish it and return to menus. Compare animation speed, collision/physics, sound and rendering with a reference run. A rendered frame alone is insufficient.
5. **Persistence and coverage:** verify save/load across application restarts, creation tools, Universe and story modes, entrances, weapons and additional match types. Track each reproducible defect and supported executable hash.
6. **Distribution:** create a repeatable build and an installation flow using user-supplied game data, then test on more than one machine. No completion date or performance improvement is established yet.

The highest uncertainties are game-specific code analysis, runtime compatibility, rendering correctness and synchronization. The next useful deliverable is a reproducible boot investigation, not a promise of a complete playable port.
