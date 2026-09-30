// Graphics pipelines of the native renderer, keyed by the draw state that
// shapes them. Keys and the driver's pipeline cache persist in
// cache/native-pipelines/ next to the executable; at startup a background
// thread rebuilds the pipelines recorded in earlier sessions, so first use in
// a match does not stutter.

#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace svr::native {

constexpr uint32_t kMaxAttributes = 16;
constexpr uint32_t kMaxStreams = 8;
// Vertex binding that feeds zeros to shader inputs a declaration leaves out.
constexpr uint32_t kDummyBinding = 15;

struct VertexAttribute {
  uint32_t location;
  uint32_t stream;
  VkFormat format;
  uint32_t offset;
};

// Plain data (zero the padding): hashed, compared and stored byte-wise.
// Shaders are named by their container hash, so a key stays valid across
// sessions as long as the converted shader cache does.
struct PipelineKey {
  uint64_t vertex_shader;
  uint64_t pixel_shader;
  uint32_t spec_constants;
  uint32_t topology;
  VkFormat color_format;
  VkFormat depth_format;
  uint32_t blend_control;
  uint32_t color_mask;
  uint32_t depth_control;
  uint32_t cull;
  uint32_t strides[kMaxStreams];
  uint32_t dummy_locations;
  uint32_t attribute_count;
  VertexAttribute attributes[kMaxAttributes];
};

namespace pipelines {

// Loads the stored cache and starts the prebuild thread. shader_cache_id
// names the converted shaders; stored keys from other conversions are
// dropped.
void Initialize(uint64_t shader_cache_id);

// The pipeline for a key, created on first use (VK_NULL_HANDLE on failure).
// Render thread.
VkPipeline Get(const PipelineKey& key);

size_t Count();

// Writes the keys and the driver's cache data if pipelines were added since
// the last save.
void Save();

// Stops the prebuild thread (before the device goes away) and saves.
void Shutdown();

}  // namespace pipelines

}  // namespace svr::native
