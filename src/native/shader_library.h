// The game's shaders, converted ahead of time by XenosRecomp, matched to the
// guest shader objects D3D creates.

#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace svr::native {

struct NativeShader {
  uint64_t hash = 0;  // XXH3 of the container, as in the shader cache
  VkShaderModule module = VK_NULL_HANDLE;
  uint32_t spec_constants_mask = 0;
  // Texture fetch slots the shader reads (all when unknown).
  uint32_t fetch_slots = UINT32_MAX;
  bool is_pixel_shader = false;
};

namespace shader_library {

// Decompresses the SPIR-V cache. False if the build has no converted shaders.
bool Initialize(const rex::ui::vulkan::VulkanDevice* device);

// Guest render thread, from the Create{Vertex,Pixel}Shader hooks: container is
// the host pointer to the shader container, guest_object the created shader.
void OnShaderCreated(const uint8_t* container, uint32_t guest_object, bool is_pixel_shader);

// The converted shader for a guest shader object, its module built on first
// use; nullptr if the object is unknown or its hash is not in the cache.
const NativeShader* Find(uint32_t guest_object);

// The container hash of a guest shader object (0 if unknown), for debugging.
uint64_t Hash(uint32_t guest_object);

// The module of a converted shader by container hash, built on first use;
// VK_NULL_HANDLE if the cache has no such shader. Any thread.
VkShaderModule Module(uint64_t hash);

// Names the converted shader cache this build carries (a hash of its data).
uint64_t CacheId();

}  // namespace shader_library

}  // namespace svr::native
