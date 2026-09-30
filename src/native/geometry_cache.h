// Vertex and index data for the native renderer's indexed draws. Guest
// buffers the game leaves alone are converted once (byte-swapped, indices
// rebased) into a device-local arena and reused while their pages stay clean
// (write_watch); buffers rewritten every frame go through the upload ring.

#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace svr::native::geometry {

bool Initialize();

struct Region {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};

// bytes of vertex data at a physical address, as 32-bit words in host order.
bool Vertices(uint32_t physical_address, uint32_t bytes, VkCommandBuffer upload_cb,
              Region& out);

// count guest indices at a physical address, rebased to the lowest index
// (32-bit, restart = all ones), quad lists split into triangles.
struct Indices {
  Region region;
  uint32_t count = 0;  // indices to draw
  uint32_t min_index = 0;
  uint32_t max_index = 0;
};
// False when no index is drawable (all restarts) or memory ran out.
bool IndexBuffer(uint32_t physical_address, uint32_t count, bool index32, bool quads,
                 VkCommandBuffer upload_cb, Indices& out);

// Bytes converted into the arena since the last call.
uint64_t TakeUploadBytes();

// Makes this frame's arena copies visible to vertex input; recorded at the
// end of the upload command buffer.
void EndFrame(VkCommandBuffer upload_cb);

}  // namespace svr::native::geometry
