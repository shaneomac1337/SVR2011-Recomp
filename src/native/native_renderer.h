// Native Vulkan renderer: draws the game from its own XDK Direct3D calls on the
// GPU plugin's device and hands each finished frame to the plugin's present
// path (rex/system/external_frame.h), replacing Xenos emulation for drawing.
// Enabled with --svr_native_renderer=true; see the Native Renderer PRD.
//
// All entry points run on the guest render thread. Each is called after the
// original D3D function, so the device's register mirror is up to date.

#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

namespace svr::native {

// Creates the renderer's resources on the runtime's Vulkan device when the
// cvar is set. Call after the graphics system is set up.
void Configure(rex::Runtime* runtime);

bool IsEnabled();

// Stops background work and saves the pipeline cache; call while the Vulkan
// device still exists.
void Shutdown();

// base is guest memory, device the guest D3DDevice.

// D3DDevice_Present: front_buffer is the texture object shown.
void OnPresent(const uint8_t* base, uint32_t device, uint32_t front_buffer);

// D3DDevice_SetRenderTarget: index and surface object.
void OnSetRenderTarget(const uint8_t* base, uint32_t index, uint32_t surface);

// D3DDevice_Clear: flags (0xF colour, 0x10 depth, 0x20 stencil), rect in
// pixels, colour as a guest float4 pointer (may be 0), depth and stencil.
void OnClear(const uint8_t* base, uint32_t device, uint32_t flags, const int32_t rect[4],
             uint32_t color, float depth, uint32_t stencil);

// D3DDevice_Resolve: flags (0..3 colour target, 4 depth-stencil) and the
// destination texture object (0 for a clear-only resolve).
void OnResolve(const uint8_t* base, uint32_t device, uint32_t flags, uint32_t destination);

// Quad and strip draws from BeginVertices / EndVertices: the vertices the
// game copied to guest address vertices.
void OnDrawVertices(const uint8_t* base, uint32_t device, uint32_t primitive,
                    uint32_t vertex_count, uint32_t stride, uint32_t vertices);

// D3DDevice_SetStreamSource: the stride, which the device does not keep.
void OnSetStreamSource(uint32_t stream, uint32_t stride);

// D3DDevice_DrawIndexedVertices: indices from the device's index buffer.
void OnDrawIndexed(const uint8_t* base, uint32_t device, uint32_t primitive,
                   int32_t base_vertex, uint32_t start_index, uint32_t index_count);

}  // namespace svr::native
