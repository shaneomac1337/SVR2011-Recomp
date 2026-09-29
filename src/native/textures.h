// Guest textures and samplers for the native renderer: fetch constants
// decoded into bindless image and sampler indices, and images that resolves
// write, standing in for the guest memory the emulator no longer fills.

#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace svr::native::textures {

bool Initialize();

// A texture fetch constant as the guest stores it (six big-endian dwords).
struct FetchConstant {
  uint32_t dwords[6];
};
FetchConstant LoadFetchConstant(const uint8_t* big_endian);

struct Binding {
  uint32_t texture_index = 0;  // in the heap matching the dimension
  uint32_t sampler_index = 0;
  uint32_t dimension = 1;  // xenos::DataDimension: 1 = 2D, 2 = 3D / stacked, 3 = cube
};

// Records guest-memory uploads into upload_cb when a texture is new or its
// data changed this frame. Unsupported textures bind the default image.
Binding Bind(const FetchConstant& fetch, VkCommandBuffer upload_cb, uint64_t frame);

// The image a resolve writes for a destination texture fetch constant,
// created on first use in TRANSFER_DST-compatible form. Later Binds of a
// fetch constant at the same address and format sample it.
struct ResolveTarget {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  VkImageLayout* layout = nullptr;
};
ResolveTarget GetResolveTarget(const FetchConstant& fetch, bool red_blue_swapped);

// The resolve target at a guest physical address, or an empty result.
ResolveTarget FindResolveTarget(uint32_t base_address);

}  // namespace svr::native::textures
