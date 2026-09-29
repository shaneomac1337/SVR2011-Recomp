// Native Vulkan renderer: draws the game from its own XDK Direct3D calls on the
// GPU plugin's device and hands each finished frame to the plugin's present
// path (rex/system/external_frame.h), replacing Xenos emulation for drawing.
// Enabled with --svr_native_renderer=true; see the Native Renderer PRD.

#pragma once

namespace rex {
class Runtime;
}

namespace svr::native {

// Creates the renderer's resources on the runtime's Vulkan device when the
// cvar is set. Call after the graphics system is set up.
void Configure(rex::Runtime* runtime);

// Guest render thread, before the original D3DDevice_Present runs.
void OnPresent();

}  // namespace svr::native
