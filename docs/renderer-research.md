# Renderer recommendation for SVR 2011

Researched 2026-09-27. Scope: Windows, AMD Radeon RX 7900 XT, ReXGlue v0.10.0 (`f5337cd`). No game launch, driver test, installation, or renderer change was performed for this research.

## Recommendation

Keep D3D12 as the baseline and prepare a separate Vulkan build for the next controlled comparison. Vulkan is a worthwhile diagnostic experiment after this project's hardware D3D12 hang; available evidence does not establish that it will be faster, more accurate, or stable for SVR 2011. Choose the eventual default from matched gameplay results.

## Evidence

- **Our failure is real but undiagnosed.** The hardware run reached the menu, then ended with `0x887A0006 DEVICE_HUNG` on driver `32.0.31041.3013`. Invalid texture-fetch warnings preceded it, without established causality. WARP produced unacceptable performance and a black screen in a later run. These are project observations, not evidence that the D3D12 API itself is unsuitable. [Local investigation](boot-investigation.md#menu-and-gameplay-rendering)
- **D3D12 is the upstream Windows default.** ReXGlue enables D3D12 and disables Vulkan by default on Windows; Vulkan is enabled on other platforms. This favors D3D12 as the existing baseline, but is not a performance benchmark. [Build configuration](https://github.com/rexglue/rexglue-sdk/blob/v0.10.0/CMakeLists.txt)
- **Windows Vulkan is a genuine supported build path.** v0.10.0 includes PR #415, fixing Windows Vulkan-only linking, and PR #413, improving Vulkan SPIR-V and texture handling for MoltenVK. These support trying our pinned version; the macOS work does not prove Windows/AMD gameplay compatibility. [Release](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0), [Windows link fix](https://github.com/rexglue/rexglue-sdk/pull/415), [Vulkan changes](https://github.com/rexglue/rexglue-sdk/pull/413)
- **Upstream Xenia advice is contextual.** Canary's options wiki, last edited January 2025, still marks Vulkan “not recommended.” It is useful historical guidance, not a current ReXGlue compatibility matrix. [Canary options](https://github.com/xenia-canary/xenia-canary/wiki/Options#rendererbackend)
- **AMD-specific precedent is old.** Closed Xenia issue #2133 reports D3D12 failing on a 7900 XT while Vulkan ran, in February 2023. Different software, drivers, games, and failure details make it a hypothesis for comparison, not our diagnosis. [Issue #2133](https://github.com/xenia-project/xenia/issues/2133)
- **Accuracy paths depend on features.** Our SDK selects host render targets by default on AMD/D3D12 and ordinarily on Vulkan. Vulkan's optional fragment-shader-interlock path checks several device capabilities and falls back when unavailable. Do not assume an old claim about AMD extension support still describes this installed driver; capabilities remain unmeasured. [D3D12 selection](https://github.com/rexglue/rexglue-sdk/blob/v0.10.0/src/graphics/d3d12/render_target_cache.cpp#L176), [Vulkan selection](https://github.com/rexglue/rexglue-sdk/blob/v0.10.0/src/graphics/vulkan/render_target_cache.cpp#L202)

## Follow-up experiment

Build the pinned SDK with Vulkan in separate SDK output/install and game build directories, preserving the D3D12 executable. The installed package has Vulkan disabled, so changing a launch argument alone cannot add it. With both backends compiled, `any` selects D3D12 first; explicitly select Vulkan and confirm the log names the hardware adapter. [Plugin selection](https://github.com/rexglue/rexglue-sdk/blob/v0.10.0/src/graphics/plugin_main.cpp#L40)

Compare the same menu/background-gameplay transition at native resolution and unchanged game settings, recording correctness, frame times, and errors. First establish whether Vulkan passes the previously failing scene; a process merely staying alive is insufficient. If both backends fail similarly, prioritize shared command/texture/memory handling. If only D3D12 fails, prioritize its shader/resource handling and the driver interaction. Neither result alone proves root cause.
