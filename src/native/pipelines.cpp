#include "native/pipelines.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/guest_decode.h"
#include "native/shader_library.h"
#include "native/vk_context.h"

REXCVAR_DEFINE_BOOL(svr_native_pipeline_cache, true, "SVR2011",
                    "Native renderer: keep pipelines in cache/native-pipelines and rebuild them "
                    "in the background at startup");
REXCVAR_DECLARE(bool, svr_native_debug_no_blend);

namespace svr::native::pipelines {

namespace {

using guest::InputLocation;
using guest::kInputLocations;

struct KeyHash {
  size_t operator()(const PipelineKey& key) const { return size_t(XXH3_64bits(&key, sizeof(key))); }
};
struct KeyEqual {
  bool operator()(const PipelineKey& a, const PipelineKey& b) const {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
  }
};

constexpr char kDirectory[] = "cache/native-pipelines";
constexpr char kKeysFile[] = "cache/native-pipelines/keys.bin";
constexpr char kCacheFile[] = "cache/native-pipelines/driver-cache.bin";

// keys.bin: this header, then count keys.
struct KeysHeader {
  char magic[4];
  uint32_t version;
  uint32_t key_size;
  uint32_t count;
  uint64_t shader_cache_id;
};
constexpr char kMagic[4] = {'S', 'V', 'R', 'P'};
constexpr uint32_t kVersion = 1;

struct State {
  std::mutex mutex;
  std::unordered_map<PipelineKey, VkPipeline, KeyHash, KeyEqual> pipelines;
  std::vector<PipelineKey> created;  // in creation order, for Save
  size_t saved = 0;
  VkPipelineCache cache = VK_NULL_HANDLE;
  uint64_t shader_cache_id = 0;
  std::atomic<uint32_t> errors_logged{0};
  std::jthread prebuild;
} g;

std::vector<uint8_t> ReadFile(const char* path) {
  std::vector<uint8_t> data;
  if (FILE* file = std::fopen(path, "rb")) {
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      data.resize(std::fread(data.data(), 1, data.size(), file));
    }
    std::fclose(file);
  }
  return data;
}

// Written beside the target and renamed over it, so a crash mid-write keeps
// the previous file.
bool WriteFile(const char* path, const void* a, size_t a_size, const void* b, size_t b_size) {
  const std::string temporary = std::string(path) + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    return false;
  }
  const bool ok = std::fwrite(a, 1, a_size, file) == a_size &&
                  (!b_size || std::fwrite(b, 1, b_size, file) == b_size);
  std::fclose(file);
  std::error_code error;
  if (ok) {
    std::filesystem::rename(temporary, path, error);
  }
  return ok && !error;
}

VkBlendFactor BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

VkStencilOpState StencilState(uint32_t func, uint32_t fail, uint32_t pass, uint32_t depth_fail) {
  VkStencilOpState state = {};
  // Xenos CompareFunction and StencilOp values equal Vulkan's.
  state.compareOp = VkCompareOp(func);
  state.failOp = VkStencilOp(fail);
  state.passOp = VkStencilOp(pass);
  state.depthFailOp = VkStencilOp(depth_fail);
  state.compareMask = 0xFF;
  state.writeMask = 0xFF;
  return state;
}

// Any thread: pipeline creation with a pipeline cache is thread-safe.
VkPipeline Create(const PipelineKey& key) {
  const VkShaderModule vertex_module = shader_library::Module(key.vertex_shader);
  const VkShaderModule pixel_module = shader_library::Module(key.pixel_shader);
  if (!vertex_module || !pixel_module) {
    return VK_NULL_HANDLE;
  }
  VkSpecializationMapEntry spec_entry = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec_info = {1, &spec_entry, sizeof(uint32_t), &key.spec_constants};
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module;
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &spec_info;
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = pixel_module;

  VkVertexInputBindingDescription bindings[kMaxStreams + 1] = {};
  uint32_t binding_count = 0;
  for (uint32_t stream = 0; stream < kMaxStreams; ++stream) {
    if (key.strides[stream]) {
      bindings[binding_count++] = {stream, key.strides[stream], VK_VERTEX_INPUT_RATE_VERTEX};
    }
  }
  if (key.dummy_locations) {
    bindings[binding_count++] = {kDummyBinding, 0, VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[kMaxAttributes * 2] = {};
  uint32_t attribute_count = 0;
  for (uint32_t i = 0; i < key.attribute_count; ++i) {
    attributes[attribute_count++] = {key.attributes[i].location, key.attributes[i].stream,
                                     key.attributes[i].format, key.attributes[i].offset};
  }
  for (const InputLocation& l : kInputLocations) {
    if (key.dummy_locations & (1u << l.location)) {
      attributes[attribute_count++] = {
          l.location, kDummyBinding,
          l.is_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
  }
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = binding_count;
  vertex_input.pVertexBindingDescriptions = bindings;
  vertex_input.vertexAttributeDescriptionCount = attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  // Strips from index buffers use the all-ones index as a restart marker.
  input_assembly.primitiveRestartEnable =
      key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
      key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
      key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = ((key.cull & 1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                    ((key.cull & 2) ? VK_CULL_MODE_BACK_BIT : 0);
  raster.frontFace = (key.cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  const uint32_t dc = key.depth_control;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  if (key.depth_format != VK_FORMAT_UNDEFINED) {
    depth_stencil.depthTestEnable = (dc >> 1) & 1;
    depth_stencil.depthWriteEnable = (dc >> 2) & 1;
    depth_stencil.depthCompareOp = VkCompareOp((dc >> 4) & 7);
    depth_stencil.stencilTestEnable = dc & 1;
    depth_stencil.front = StencilState((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7,
                                       (dc >> 17) & 7);
    depth_stencil.back = ((dc >> 7) & 1) ? StencilState((dc >> 20) & 7, (dc >> 23) & 7,
                                                        (dc >> 26) & 7, (dc >> 29) & 7)
                                         : depth_stencil.front;
  }

  const uint32_t bc = key.blend_control;
  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.srcColorBlendFactor = BlendFactor(bc & 0x1F);
  blend_attachment.colorBlendOp = BlendOp((bc >> 5) & 7);
  blend_attachment.dstColorBlendFactor = BlendFactor((bc >> 8) & 0x1F);
  blend_attachment.srcAlphaBlendFactor = BlendFactor((bc >> 16) & 0x1F);
  blend_attachment.alphaBlendOp = BlendOp((bc >> 21) & 7);
  blend_attachment.dstAlphaBlendFactor = BlendFactor((bc >> 24) & 0x1F);
  // ONE, ZERO, ADD on both is the Xenos way of saying "no blending".
  blend_attachment.blendEnable =
      (bc & 0x1FFF1FFF) != 0x00010001 && !REXCVAR_GET(svr_native_debug_no_blend);
  blend_attachment.colorWriteMask = key.color_mask & 0xF;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &key.color_format;
  rendering.depthAttachmentFormat = key.depth_format;
  rendering.stencilAttachmentFormat = key.depth_format;

  VkGraphicsPipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.pNext = &rendering;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = g_vk.pipeline_layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (g_vk.dfn->vkCreateGraphicsPipelines(g_vk.vk_device, g.cache, 1, &pipeline_info, nullptr,
                                          &pipeline) != VK_SUCCESS) {
    if (g.errors_logged++ < 16) {
      REXLOG_ERROR("native renderer: vkCreateGraphicsPipelines failed");
    }
    pipeline = VK_NULL_HANDLE;
  }
  return pipeline;
}

// Stores a created pipeline unless another thread stored one first.
VkPipeline Insert(const PipelineKey& key, VkPipeline pipeline) {
  std::lock_guard lock(g.mutex);
  const auto [it, inserted] = g.pipelines.emplace(key, pipeline);
  if (!inserted) {
    if (pipeline) {
      g_vk.dfn->vkDestroyPipeline(g_vk.vk_device, pipeline, nullptr);
    }
    return it->second;
  }
  if (pipeline) {
    g.created.push_back(key);
  }
  return pipeline;
}

std::vector<PipelineKey> LoadKeys() {
  const std::vector<uint8_t> data = ReadFile(kKeysFile);
  KeysHeader header;
  if (data.size() < sizeof(header)) {
    return {};
  }
  std::memcpy(&header, data.data(), sizeof(header));
  if (std::memcmp(header.magic, kMagic, 4) != 0 || header.version != kVersion ||
      header.key_size != sizeof(PipelineKey) || header.shader_cache_id != g.shader_cache_id ||
      data.size() < sizeof(header) + size_t(header.count) * sizeof(PipelineKey)) {
    REXLOG_INFO("native renderer: stored pipelines are from other shaders or another build; "
                "starting over");
    return {};
  }
  std::vector<PipelineKey> keys(header.count);
  std::memcpy(keys.data(), data.data() + sizeof(header), keys.size() * sizeof(PipelineKey));
  return keys;
}

}  // namespace

void Initialize(uint64_t shader_cache_id) {
  g.shader_cache_id = shader_cache_id;
  if (!REXCVAR_GET(svr_native_pipeline_cache)) {
    return;
  }
  // The driver checks the data's header (vendor, device, cache UUID) and
  // ignores data from another GPU or driver.
  const std::vector<uint8_t> initial = ReadFile(kCacheFile);
  VkPipelineCacheCreateInfo cache_info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  cache_info.initialDataSize = initial.size();
  cache_info.pInitialData = initial.empty() ? nullptr : initial.data();
  if (g_vk.vkCreatePipelineCache(g_vk.vk_device, &cache_info, nullptr, &g.cache) !=
      VK_SUCCESS) {
    cache_info.initialDataSize = 0;
    cache_info.pInitialData = nullptr;
    if (g_vk.vkCreatePipelineCache(g_vk.vk_device, &cache_info, nullptr, &g.cache) !=
        VK_SUCCESS) {
      g.cache = VK_NULL_HANDLE;
      return;
    }
  }
  std::vector<PipelineKey> keys = LoadKeys();
  if (keys.empty()) {
    return;
  }
  REXLOG_INFO("native renderer: prebuilding {} stored pipelines", keys.size());
  g.prebuild = std::jthread([keys = std::move(keys)](std::stop_token stop) {
    size_t built = 0;
    for (const PipelineKey& key : keys) {
      if (stop.stop_requested()) {
        return;
      }
      {
        std::lock_guard lock(g.mutex);
        if (g.pipelines.count(key)) {
          continue;
        }
      }
      built += Insert(key, Create(key)) != VK_NULL_HANDLE;
    }
    REXLOG_INFO("native renderer: prebuilt {} of {} stored pipelines", built, keys.size());
  });
}

VkPipeline Get(const PipelineKey& key) {
  {
    std::lock_guard lock(g.mutex);
    const auto it = g.pipelines.find(key);
    if (it != g.pipelines.end()) {
      return it->second;
    }
  }
  return Insert(key, Create(key));
}

size_t Count() {
  std::lock_guard lock(g.mutex);
  return g.pipelines.size();
}

void Save() {
  if (!g.cache) {
    return;
  }
  std::vector<PipelineKey> keys;
  {
    std::lock_guard lock(g.mutex);
    if (g.created.size() == g.saved) {
      return;
    }
    keys = g.created;
    g.saved = keys.size();
  }
  std::error_code error;
  std::filesystem::create_directories(kDirectory, error);
  KeysHeader header = {};
  std::memcpy(header.magic, kMagic, 4);
  header.version = kVersion;
  header.key_size = sizeof(PipelineKey);
  header.count = uint32_t(keys.size());
  header.shader_cache_id = g.shader_cache_id;
  size_t size = 0;
  std::vector<uint8_t> data;
  if (g_vk.vkGetPipelineCacheData(g_vk.vk_device, g.cache, &size, nullptr) == VK_SUCCESS &&
      size) {
    data.resize(size);
    if (g_vk.vkGetPipelineCacheData(g_vk.vk_device, g.cache, &size, data.data()) !=
        VK_SUCCESS) {
      size = 0;
    }
    data.resize(size);
  }
  const bool ok = WriteFile(kKeysFile, &header, sizeof(header), keys.data(),
                            keys.size() * sizeof(PipelineKey)) &&
                  (data.empty() || WriteFile(kCacheFile, data.data(), data.size(), nullptr, 0));
  REXLOG_INFO("native renderer: {} {} pipelines ({} KB driver cache)",
              ok ? "saved" : "could not save", keys.size(), data.size() / 1024);
}

}  // namespace svr::native::pipelines
