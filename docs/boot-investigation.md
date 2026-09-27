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

## Remaining work

Track subsequent runtime results here. Do not describe a successful compile or
a process surviving the smoke timeout as proof of a playable game. Menu and match
milestones require separate visual and interaction verification.
