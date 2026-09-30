#include "native/native_renderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/external_frame.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#include "native/geometry_cache.h"
#include "native/guest_decode.h"
#include "native/shader_library.h"
#include "native/textures.h"
#include "native/vk_context.h"
#include "native/write_watch.h"

REXCVAR_DEFINE_BOOL(svr_native_renderer, false, "SVR2011",
                    "Draw with the native Vulkan renderer instead of Xenos emulation "
                    "(experimental)");
REXCVAR_DEFINE_INT32(svr_native_debug_target, 0, "SVR2011",
                     "Native renderer debugging: present the Nth render target created (1-based) "
                     "instead of the front buffer");
REXCVAR_DEFINE_INT32(svr_native_trace_frame, -1, "SVR2011",
                     "Native renderer debugging: log every event of this frame number");
REXCVAR_DEFINE_UINT32(svr_native_debug_resolve, 0, "SVR2011",
                      "Native renderer debugging: present the resolve target at this physical "
                      "address instead of the front buffer");
REXCVAR_DEFINE_UINT32(svr_native_debug_null_slots, 0, "SVR2011",
                      "Native renderer debugging: bitmask of texture fetch slots bound to the "
                      "default texture");
REXCVAR_DEFINE_BOOL(svr_native_debug_no_blend, false, "SVR2011",
                    "Native renderer debugging: disable blending");
REXCVAR_DEFINE_UINT64(svr_native_trace_ps, 0, "SVR2011",
                      "Native renderer debugging: in traced frames, dump the constants of draws "
                      "using this pixel shader hash");
REXCVAR_DEFINE_BOOL(svr_native_half_pixel_offset, true, "SVR2011",
                    "Native renderer: shift geometry by half a pixel for D3D9-style pixel "
                    "centres, like the emulated path's half_pixel_offset");
REXCVAR_DEFINE_BOOL(svr_native_reuse_constants, true, "SVR2011",
                    "Native renderer: upload shader constants only when they differ from the "
                    "previous draw's");
REXCVAR_DEFINE_BOOL(svr_native_bind_used_slots, true, "SVR2011",
                    "Native renderer: bind textures only in the fetch slots the draw's shaders "
                    "read (from cache/shader-native/fetch_slots.cpp)");
REXCVAR_DEFINE_BOOL(svr_native_swap_half2, true, "SVR2011",
                    "Native renderer: 16-bit texcoords have their halves swapped after the "
                    "32-bit vertex byte swap");

namespace svr::native {

namespace {

using namespace guest;
using rex::ui::vulkan::VulkanDevice;
namespace vk_util = rex::ui::vulkan::util;

// The plugin samples a published frame after our submission, so three slots
// keep a frame we are writing apart from the one being presented.
constexpr uint32_t kFramesInFlight = 3;
constexpr VkDeviceSize kUploadRingSize = 64ull << 20;

// Guest D3DDevice layout (SvR 2011's XDK), verified with --svr_d3d_probe.
constexpr uint32_t kDeviceFetchConstants = 0x480;  // 32 x 6 dwords
constexpr uint32_t kDeviceVertexConstants = 0x780;  // 256 float4
constexpr uint32_t kDevicePixelConstants = 0x1780;  // 256 float4
constexpr uint32_t kDeviceBooleans = 0x2780;        // VS 4 dwords, PS 4 dwords
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;
constexpr uint32_t kDeviceStreamFetch = 0x778;  // stream 0; stream s at -8 s
constexpr uint32_t kDeviceIndexBuffer = 0x3144;
constexpr uint32_t kDevicePixelShader = 0x3244;
constexpr uint32_t kDeviceVertexShader = 0x3248;
// Texture objects keep their fetch constant here.
constexpr uint32_t kTextureFetchConstant = 0x1C;
// Surface objects: packed size (DecodeSurfaceSize).
constexpr uint32_t kSurfaceSize = 0x24;

enum Reg : uint32_t {
  RB_SURFACE_INFO = 0x2000,
  RB_COLOR_INFO = 0x2001,
  RB_DEPTH_INFO = 0x2002,
  PA_SC_SCREEN_SCISSOR_TL = 0x200E,
  PA_SC_SCREEN_SCISSOR_BR = 0x200F,
  RB_COLOR_MASK = 0x2104,
  RB_BLEND_RED = 0x2105,
  RB_STENCILREFMASK_BF = 0x210C,
  RB_STENCILREFMASK = 0x210D,
  RB_ALPHA_REF = 0x210E,
  PA_CL_VPORT_XSCALE = 0x210F,
  PA_CL_VPORT_XOFFSET = 0x2110,
  PA_CL_VPORT_YSCALE = 0x2111,
  PA_CL_VPORT_YOFFSET = 0x2112,
  PA_CL_VPORT_ZSCALE = 0x2113,
  PA_CL_VPORT_ZOFFSET = 0x2114,
  RB_DEPTHCONTROL = 0x2200,
  RB_BLENDCONTROL0 = 0x2201,
  RB_COLORCONTROL = 0x2202,
  PA_SU_SC_MODE_CNTL = 0x2205,
  PA_CL_VTE_CNTL = 0x2206,
  PA_SU_VTX_CNTL = 0x2302,
  RB_COPY_CONTROL = 0x2318,
  RB_COPY_DEST_INFO = 0x231B,
  RB_DEPTH_CLEAR = 0x231D,
  RB_COLOR_CLEAR = 0x231E,
};

bool Tracing();

#define TRACE(...)                                              \
  do {                                                          \
    if (Tracing()) REXLOG_INFO("native trace: " __VA_ARGS__); \
  } while (0)

// --- Vertex declarations ----------------------------------------------------

constexpr uint32_t kMaxAttributes = 16;
constexpr uint32_t kDummyBinding = 15;

struct VertexAttribute {
  uint32_t location;
  uint32_t stream;
  VkFormat format;
  uint32_t offset;
};

constexpr uint32_t kMaxStreams = 8;

struct VertexLayout {
  uint32_t attribute_count = 0;
  VertexAttribute attributes[kMaxAttributes] = {};
  uint32_t dummy_locations = 0;  // bitmask of shader locations fed zeros
  uint32_t swapped_texcoords = 0;
  uint32_t stream_mask = 0;
  bool packed_normal = false;
};

struct DeclEntry {
  bool valid = false;
  VertexLayout layout;
};

// Declaration objects (verified): element count at +0x18, D3DVERTEXELEMENT9
// array at +0x34 (12 bytes each, big-endian: stream, offset, type, method,
// usage, usage index).
constexpr uint32_t kDeclarationCount = 0x18;
constexpr uint32_t kDeclarationElements = 0x34;

const DeclEntry* GetDeclaration(const uint8_t* base, uint32_t object) {
  static std::unordered_map<uint64_t, DeclEntry> cache;
  static uint32_t logged = 0;
  if (!object) {
    return nullptr;
  }
  const uint8_t* p = base + object;
  const uint32_t count = std::min<uint32_t>(LoadBE32(p + kDeclarationCount), kMaxAttributes);
  const uint8_t* elements = p + kDeclarationElements;
  const uint64_t key = uint64_t(object) << 32 ^ XXH3_64bits(elements, count * 12);
  auto [it, inserted] = cache.try_emplace(key);
  DeclEntry& entry = it->second;
  if (!inserted) {
    return entry.valid ? &entry : nullptr;
  }
  VertexLayout& layout = entry.layout;
  std::string description;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = elements + i * 12;
    const uint16_t stream = LoadBE16(e);
    const uint16_t offset = LoadBE16(e + 2);
    const uint32_t type = LoadBE32(e + 4);
    const uint8_t usage = e[9];
    const uint8_t usage_index = e[10];
    description += fmt::format(" {}.{}@{}+{}:{:X}", usage, usage_index, stream, offset, type);
    const DeclType* decl_type = FindDeclType(type);
    const InputLocation* location = FindLocation(usage, usage_index);
    if (!decl_type || stream >= kMaxStreams) {
      REXLOG_WARN("native renderer: declaration {:08X} element {} has unsupported type {:X} "
                  "(stream {})",
                  object, i, type, stream);
      continue;
    }
    if (!location) {
      continue;
    }
    VertexAttribute& attribute = layout.attributes[layout.attribute_count++];
    attribute.location = location->location;
    attribute.format = location->is_uint ? decl_type->uint_format : decl_type->float_format;
    attribute.offset = offset;
    attribute.stream = stream;
    layout.stream_mask |= 1u << stream;
    if (decl_type->packed_normal && location->is_uint) {
      layout.packed_normal = true;
    }
    if (usage == 5 && decl_type->sixteen_bit && REXCVAR_GET(svr_native_swap_half2)) {
      layout.swapped_texcoords |= 1u << usage_index;
    }
  }
  entry.valid = layout.attribute_count > 0;
  for (const InputLocation& l : kInputLocations) {
    bool present = false;
    for (uint32_t i = 0; i < entry.layout.attribute_count; ++i) {
      present |= entry.layout.attributes[i].location == l.location;
    }
    if (!present) {
      entry.layout.dummy_locations |= 1u << l.location;
    }
  }
  if (logged++ < 64) {
    REXLOG_INFO("native renderer: declaration {:08X} (usage.index@stream+offset:type):{}",
                object, description);
  }
  return entry.valid ? &entry : nullptr;
}

// --- Render targets ---------------------------------------------------------

struct RenderTarget {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool is_depth = false;
};

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

// --- Pipelines --------------------------------------------------------------

struct PipelineKey {
  VkShaderModule vertex_shader;
  VkShaderModule pixel_shader;
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

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const { return size_t(XXH3_64bits(&key, sizeof(key))); }
};
struct PipelineKeyEqual {
  bool operator()(const PipelineKey& a, const PipelineKey& b) const {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
  }
};

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

// --- Renderer state ---------------------------------------------------------

struct FrameSlot {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer upload_cb = VK_NULL_HANDLE;
  VkCommandBuffer main_cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};

struct Stats {
  uint64_t drawn = 0;
  uint64_t no_shader = 0;
  uint64_t no_declaration = 0;
  uint64_t unsupported_primitive = 0;
  uint64_t drawn_indexed = 0;
  uint64_t bad_stream = 0;
  uint64_t other = 0;
  uint64_t clears = 0;
  uint64_t resolves = 0;
};

struct State {
  const VulkanDevice* device = nullptr;
  FrameSlot slots[kFramesInFlight];
  uint32_t width = 1280;
  uint32_t height = 720;
  uint64_t frame = 0;
  bool recording = false;
  // Blocks of this frame's upload ring that later draws reuse while their
  // contents do not change (reset with the ring each frame).
  struct ReusedBlock {
    uint8_t last[4096];
    VkDeviceAddress address = 0;
  };
  ReusedBlock vertex_constants, pixel_constants;
  SharedConstants last_shared = {};
  VkDeviceAddress shared_address = 0;
  PushConstants last_push = {};
  Upload zeros;
  FrameSlot* slot = nullptr;

  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> render_targets;
  bool rendering = false;
  RenderTarget* active_color = nullptr;
  RenderTarget* active_depth = nullptr;
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  // Size of the surfaces set with SetRenderTarget, per index.
  uint32_t surface_width[4] = {1280, 1280, 1280, 1280};
  uint32_t surface_height[4] = {720, 720, 720, 720};

  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash, PipelineKeyEqual> pipelines;
  uint32_t stream_strides[kMaxStreams] = {};
  // Resolve destinations: frame of the last resolve and the start of the
  // current run of consecutive frames.
  struct ResolveRun {
    uint64_t last_frame = 0;
    uint64_t run_start = 0;
  };
  std::unordered_map<uint32_t, ResolveRun> resolve_runs;
  struct PendingReadback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    textures::FetchConstant fetch;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes_per_pixel = 0;
    uint64_t frame = 0;
  };
  std::vector<PendingReadback> pending_readbacks;
  // Staging for depth resolves (depth aspect -> buffer -> R32 image).
  VkBuffer depth_staging = VK_NULL_HANDLE;
  VkDeviceMemory depth_staging_memory = VK_NULL_HANDLE;
  VkDeviceSize depth_staging_size = 0;
  uint64_t readbacks_delivered = 0;
  std::vector<RenderTarget*> render_target_order;
  Stats stats;
  uint32_t errors_logged = 0;
} g;

int64_t g_trace_frame = -1;

bool Tracing() {
  return int64_t(g.frame) == int64_t(REXCVAR_GET(svr_native_trace_frame)) ||
         int64_t(g.frame) == g_trace_frame;
}

// Debugging: creating native_trace_request next to the executable traces the
// next frame (checked twice a second).
void PollTraceRequest() {
  if (g.frame % 30 != 0) {
    return;
  }
  std::error_code error;
  if (std::filesystem::remove("native_trace_request", error)) {
    g_trace_frame = int64_t(g.frame) + 1;
    REXLOG_INFO("native renderer: tracing frame {}", g_trace_frame);
  }
}

void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout old_layout, VkImageLayout new_layout) {
  // Conservative full barrier; the frame has few layout changes.
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = vk_util::InitializeSubresourceRange(aspect);
  g_vk.dfn->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
}

void TransitionTarget(RenderTarget& target, VkImageLayout layout) {
  if (target.layout == layout) {
    return;
  }
  ImageBarrier(g.slot->main_cb, target.image,
               target.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_COLOR_BIT,
               target.layout, layout);
  target.layout = layout;
}

bool CreateFrameSlot(FrameSlot& slot) {
  const VulkanDevice::Functions& dfn = *g_vk.dfn;
  const VkDevice device = g_vk.vk_device;

  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {g.width, g.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_util::CreateDedicatedAllocationImage(g.device, image_info,
                                               vk_util::MemoryPurpose::kDeviceLocal, slot.image,
                                               slot.memory)) {
    return false;
  }

  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = slot.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = vk_util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &slot.view) != VK_SUCCESS) {
    return false;
  }

  // One pool per slot, reset whole once the slot's fence has signalled.
  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = g.device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBuffer command_buffers[2];
  VkCommandBufferAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = slot.command_pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 2;
  if (dfn.vkAllocateCommandBuffers(device, &allocate_info, command_buffers) != VK_SUCCESS) {
    return false;
  }
  slot.upload_cb = command_buffers[0];
  slot.main_cb = command_buffers[1];

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  return dfn.vkCreateFence(device, &fence_info, nullptr, &slot.fence) == VK_SUCCESS;
}

// Writes resolve copies whose frame the GPU has finished into guest memory,
// in submission order, so the game's CPU code sees render-to-texture results
// (created attire bakes read them back). Never waits.
void DeliverReadbacks() {
  const VulkanDevice::Functions& dfn = *g_vk.dfn;
  size_t delivered = 0;
  for (const State::PendingReadback& p : g.pending_readbacks) {
    // A slot's fence is only reset when the slot records again, three frames
    // on, so until then its status is that of frame p.frame.
    const bool done =
        g.frame >= p.frame + kFramesInFlight ||
        dfn.vkGetFenceStatus(g_vk.vk_device, g.slots[p.frame % kFramesInFlight].fence) ==
            VK_SUCCESS;
    if (!done) {
      break;
    }
    void* mapped = nullptr;
    if (dfn.vkMapMemory(g_vk.vk_device, p.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
      VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = p.memory;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(g_vk.vk_device, 1, &range);
      textures::WriteToGuest(p.fetch, static_cast<const uint8_t*>(mapped), p.width, p.height);
      dfn.vkUnmapMemory(g_vk.vk_device, p.memory);
    }
    dfn.vkDestroyBuffer(g_vk.vk_device, p.buffer, nullptr);
    dfn.vkFreeMemory(g_vk.vk_device, p.memory, nullptr);
    ++delivered;
  }
  g.pending_readbacks.erase(g.pending_readbacks.begin(),
                            g.pending_readbacks.begin() + delivered);
  g.readbacks_delivered += delivered;
}

// Copies the first resolve in a run of consecutive frames to a readback
// buffer (burst readback): one-off render-to-texture results reach guest
// memory, per-frame targets cost nothing after their first frame.
void QueueReadback(const textures::FetchConstant& fetch, const textures::ResolveTarget& target,
                   VkImageLayout& layout) {
  const uint32_t address = fetch.dwords[1] & 0xFFFFF000;
  State::ResolveRun& run = g.resolve_runs[address];
  if (run.last_frame + 1 < g.frame || run.last_frame == 0) {
    run.run_start = g.frame;
  }
  run.last_frame = g.frame;
  const uint32_t bytes_per_pixel = textures::GuestBytesPerPixel(fetch);
  if (run.run_start != g.frame || !bytes_per_pixel) {
    return;
  }
  State::PendingReadback pending;
  pending.fetch = fetch;
  pending.width = target.width;
  pending.height = target.height;
  pending.bytes_per_pixel = bytes_per_pixel;
  pending.frame = g.frame;
  if (!vk_util::CreateDedicatedAllocationBuffer(
          g.device, VkDeviceSize(target.width) * target.height * bytes_per_pixel,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk_util::MemoryPurpose::kReadback, pending.buffer,
          pending.memory)) {
    return;
  }
  const VkCommandBuffer cb = g.slot->main_cb;
  ImageBarrier(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {target.width, target.height, 1};
  g_vk.dfn->vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   pending.buffer, 1, &region);
  g.pending_readbacks.push_back(pending);
}

void BeginFrameIfNeeded() {
  if (g.recording) {
    return;
  }
  DeliverReadbacks();
  const VulkanDevice::Functions& dfn = *g_vk.dfn;
  const uint32_t slot_index = uint32_t(g.frame % kFramesInFlight);
  FrameSlot& slot = g.slots[slot_index];
  dfn.vkWaitForFences(g_vk.vk_device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(g_vk.vk_device, 1, &slot.fence);
  // The slot's previous frame, and every frame before it, has completed.
  g_vk.frame = g.frame;
  if (g.frame >= kFramesInFlight) {
    DestroyRetired(g.frame - kFramesInFlight);
  }
  if (g.frame % 300 == 0) {
    textures::EvictUnused(g.frame);
  }
  dfn.vkResetCommandPool(g_vk.vk_device, slot.command_pool, 0);
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.upload_cb, &begin_info);
  dfn.vkBeginCommandBuffer(slot.main_cb, &begin_info);
  BeginUploadFrame(slot_index);
  g.vertex_constants.address = g.pixel_constants.address = g.shared_address = 0;
  g.last_push = {};
  g.zeros = {};
  BindDescriptorHeaps(slot.main_cb);
  g.slot = &slot;
  g.recording = true;
  g.bound_pipeline = VK_NULL_HANDLE;
}

void EndRendering() {
  if (g.rendering) {
    g_vk.dfn->vkCmdEndRendering(g.slot->main_cb);
    g.rendering = false;
  }
}

RenderTarget* GetRenderTarget(uint64_t key, VkFormat format, uint32_t width, uint32_t height,
                              bool is_depth) {
  std::unique_ptr<RenderTarget>& slot = g.render_targets[key];
  if (slot && (slot->width < width || slot->height < height)) {
    // Grows in place of the old one, which frames in flight may still use.
    if (g.active_color == slot.get() || g.active_depth == slot.get()) {
      EndRendering();
      g.active_color = g.active_depth = nullptr;
    }
    std::erase(g.render_target_order, slot.get());
    Retired retired;
    retired.image = slot->image;
    retired.memory = slot->memory;
    retired.views.push_back(slot->view);
    Retire(std::move(retired));
    slot.reset();
  }
  if (slot) {
    return slot.get();
  }
  auto target = std::make_unique<RenderTarget>();
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     (is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                               : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_util::CreateDedicatedAllocationImage(g.device, image_info,
                                               vk_util::MemoryPurpose::kDeviceLocal,
                                               target->image, target->memory)) {
    REXLOG_ERROR("native renderer: could not create a {}x{} render target", width, height);
    g.render_targets.erase(key);
    return nullptr;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = target->image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = vk_util::InitializeSubresourceRange(
      is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
               : VK_IMAGE_ASPECT_COLOR_BIT);
  g_vk.dfn->vkCreateImageView(g_vk.vk_device, &view_info, nullptr, &target->view);
  target->format = format;
  target->width = width;
  target->height = height;
  target->is_depth = is_depth;
  slot = std::move(target);
  g.render_target_order.push_back(slot.get());
  REXLOG_INFO("native renderer: {} render target {}x{} format {} (key {:X})",
              is_depth ? "depth" : "colour", width, height, uint32_t(format), key);
  return slot.get();
}

RenderTarget* CurrentColorTarget(const uint8_t* d3d, bool create) {
  const uint32_t surface_info = ReadReg(d3d, RB_SURFACE_INFO);
  const uint32_t color_info = ReadReg(d3d, RB_COLOR_INFO);
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint64_t key = uint64_t(color_info & 0xF0FFF) | uint64_t(pitch) << 32;
  const uint32_t width = pitch ? pitch : g.surface_width[0];
  const uint32_t height = g.surface_height[0];
  if (!create) {
    const auto it = g.render_targets.find(key);
    return it != g.render_targets.end() ? it->second.get() : nullptr;
  }
  return GetRenderTarget(key, ColorTargetFormat((color_info >> 16) & 0xF), width, height, false);
}

RenderTarget* CurrentDepthTarget(const uint8_t* d3d, uint32_t width, uint32_t height) {
  const uint32_t surface_info = ReadReg(d3d, RB_SURFACE_INFO);
  const uint32_t depth_info = ReadReg(d3d, RB_DEPTH_INFO);
  const uint64_t key =
      uint64_t(depth_info & 0x10FFF) | uint64_t(surface_info & 0x3FFF) << 32 | 1ull << 63;
  return GetRenderTarget(key, kDepthFormat, width, height, true);
}

// Begins dynamic rendering into the guest's current render targets unless it
// is already active with them.
bool EnsureRendering(const uint8_t* d3d, bool need_depth) {
  RenderTarget* color = CurrentColorTarget(d3d, true);
  if (!color) {
    return false;
  }
  RenderTarget* depth =
      need_depth ? CurrentDepthTarget(d3d, color->width, color->height) : nullptr;
  if (g.rendering && g.active_color == color && (!need_depth || g.active_depth == depth)) {
    return true;
  }
  EndRendering();
  TransitionTarget(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (depth) {
    TransitionTarget(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  }
  VkRenderingAttachmentInfo color_attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color_attachment.imageView = color->view;
  color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingAttachmentInfo depth_attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth) {
    depth_attachment.imageView = depth->view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingInfo rendering_info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering_info.renderArea = {{0, 0}, {color->width, color->height}};
  rendering_info.layerCount = 1;
  rendering_info.colorAttachmentCount = 1;
  rendering_info.pColorAttachments = &color_attachment;
  rendering_info.pDepthAttachment = depth ? &depth_attachment : nullptr;
  rendering_info.pStencilAttachment = depth ? &depth_attachment : nullptr;
  g_vk.dfn->vkCmdBeginRendering(g.slot->main_cb, &rendering_info);
  g.rendering = true;
  g.active_color = color;
  g.active_depth = depth;
  TRACE("begin rendering colour {}x{} fmt {} info {:08X} surface {:08X} depth {}", color->width,
        color->height, uint32_t(color->format), ReadReg(d3d, RB_COLOR_INFO),
        ReadReg(d3d, RB_SURFACE_INFO), depth ? "yes" : "no");
  g.bound_pipeline = VK_NULL_HANDLE;
  return true;
}

VkPipeline GetPipeline(const PipelineKey& key) {
  const auto it = g.pipelines.find(key);
  if (it != g.pipelines.end()) {
    return it->second;
  }
  VkSpecializationMapEntry spec_entry = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec_info = {1, &spec_entry, sizeof(uint32_t), &key.spec_constants};
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = key.vertex_shader;
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &spec_info;
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = key.pixel_shader;

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
  if (g_vk.dfn->vkCreateGraphicsPipelines(g_vk.vk_device, VK_NULL_HANDLE, 1, &pipeline_info,
                                          nullptr, &pipeline) != VK_SUCCESS) {
    if (g.errors_logged++ < 16) {
      REXLOG_ERROR("native renderer: vkCreateGraphicsPipelines failed");
    }
    pipeline = VK_NULL_HANDLE;
  }
  g.pipelines.emplace(key, pipeline);
  return pipeline;
}

void SetDynamicState(const uint8_t* d3d, VkCommandBuffer cb) {
  const auto& dfn = *g_vk.dfn;
  const uint32_t vte = ReadReg(d3d, PA_CL_VTE_CNTL);
  const float x_scale = (vte & 1) ? ReadRegFloat(d3d, PA_CL_VPORT_XSCALE) : 1.0f;
  const float x_offset = (vte & 2) ? ReadRegFloat(d3d, PA_CL_VPORT_XOFFSET) : 0.0f;
  const float y_scale = (vte & 4) ? ReadRegFloat(d3d, PA_CL_VPORT_YSCALE) : 1.0f;
  const float y_offset = (vte & 8) ? ReadRegFloat(d3d, PA_CL_VPORT_YOFFSET) : 0.0f;
  const float z_scale = (vte & 16) ? ReadRegFloat(d3d, PA_CL_VPORT_ZSCALE) : 1.0f;
  const float z_offset = (vte & 32) ? ReadRegFloat(d3d, PA_CL_VPORT_ZOFFSET) : 0.0f;
  // The converted vertex shaders negate y (-fvk-invert-y), so D3D's negative
  // y scale becomes a positive Vulkan viewport height.
  VkViewport viewport;
  viewport.x = x_offset - x_scale;
  viewport.width = 2.0f * x_scale;
  viewport.y = y_offset + y_scale;
  viewport.height = -2.0f * y_scale;
  viewport.minDepth = std::clamp(z_offset, 0.0f, 1.0f);
  viewport.maxDepth = std::clamp(z_offset + z_scale, 0.0f, 1.0f);
  if (viewport.width <= 0.0f) {
    viewport.x = 0.0f;
    viewport.width = float(g.active_color->width);
  }
  if (viewport.height < 0.0f) {
    viewport.y += viewport.height;
    viewport.height = -viewport.height;
  }
  dfn.vkCmdSetViewport(cb, 0, 1, &viewport);

  const uint32_t tl = ReadReg(d3d, PA_SC_SCREEN_SCISSOR_TL);
  const uint32_t br = ReadReg(d3d, PA_SC_SCREEN_SCISSOR_BR);
  int32_t left = int32_t(tl & 0x7FFF), top = int32_t((tl >> 16) & 0x7FFF);
  int32_t right = int32_t(br & 0x7FFF), bottom = int32_t((br >> 16) & 0x7FFF);
  right = std::min<int32_t>(right, int32_t(g.active_color->width));
  bottom = std::min<int32_t>(bottom, int32_t(g.active_color->height));
  VkRect2D scissor;
  if (right > left && bottom > top) {
    scissor = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
  } else {
    scissor = {{0, 0}, {g.active_color->width, g.active_color->height}};
  }
  dfn.vkCmdSetScissor(cb, 0, 1, &scissor);

  float blend_constants[4];
  for (uint32_t i = 0; i < 4; ++i) {
    blend_constants[i] = ReadRegFloat(d3d, RB_BLEND_RED + i);
  }
  dfn.vkCmdSetBlendConstants(cb, blend_constants);
  const uint32_t ref_mask = ReadReg(d3d, RB_STENCILREFMASK);
  const uint32_t ref_mask_bf = ReadReg(d3d, RB_STENCILREFMASK_BF);
  dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, ref_mask & 0xFF);
  dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (ref_mask >> 8) & 0xFF);
  dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (ref_mask >> 16) & 0xFF);
  dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, ref_mask_bf & 0xFF);
  dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (ref_mask_bf >> 8) & 0xFF);
  dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (ref_mask_bf >> 16) & 0xFF);
}

// Vertex, pixel and shared constants for a draw; false if the ring is full.
// 256 float4 constants (big-endian in the device), converted into the upload
// ring unless the previous draw's block holds the same values.
bool UploadConstantBlock(State::ReusedBlock& block, const uint8_t* big_endian,
                         VkDeviceAddress& address) {
  if (block.address && REXCVAR_GET(svr_native_reuse_constants) &&
      std::memcmp(block.last, big_endian, sizeof(block.last)) == 0) {
    address = block.address;
    return true;
  }
  const Upload upload = AllocateUpload(sizeof(block.last), 16);
  if (!upload.data) {
    return false;
  }
  SwapCopy32(reinterpret_cast<uint32_t*>(upload.data), big_endian, sizeof(block.last) / 4);
  std::memcpy(block.last, big_endian, sizeof(block.last));
  block.address = address = upload.address;
  return true;
}

bool UploadConstants(const uint8_t* d3d, uint32_t swapped_texcoords, uint32_t fetch_slots,
                     PushConstants& push) {
  if (!UploadConstantBlock(g.vertex_constants, d3d + kDeviceVertexConstants,
                           push.vertex_constants) ||
      !UploadConstantBlock(g.pixel_constants, d3d + kDevicePixelConstants,
                           push.pixel_constants)) {
    return false;
  }
  SharedConstants shared_values = {};
  SharedConstants* shared = &shared_values;
  for (uint32_t slot = 0; slot < kFetchSlots; ++slot) {
    const uint8_t* fetch_data = d3d + kDeviceFetchConstants + slot * 24;
    if (!(fetch_slots & (1u << slot)) || (LoadBE32(fetch_data) & 3) != 2 ||
        (REXCVAR_GET(svr_native_debug_null_slots) & (1u << slot))) {
      continue;
    }
    TRACE("  slot {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", slot, LoadBE32(fetch_data),
          LoadBE32(fetch_data + 4), LoadBE32(fetch_data + 8), LoadBE32(fetch_data + 12),
          LoadBE32(fetch_data + 16), LoadBE32(fetch_data + 20));
    const textures::Binding binding = textures::Bind(textures::LoadFetchConstant(fetch_data),
                                                     g.slot->upload_cb, g.frame);
    switch (binding.dimension) {
      case 2: shared->texture_3d[slot] = binding.texture_index; break;
      case 3: shared->texture_cube[slot] = binding.texture_index; break;
      default: shared->texture_2d[slot] = binding.texture_index; break;
    }
    shared->sampler[slot] = binding.sampler_index;
  }
  SwapCopy32(shared->booleans, d3d + kDeviceBooleans, 8);
  shared->swapped_texcoords = swapped_texcoords;
  shared->alpha_threshold = ReadRegFloat(d3d, RB_ALPHA_REF);
  // Pixel centres at .0 (PA_SU_VTX_CNTL pix_center 0, as in D3D9): shift
  // geometry by half a pixel right and down, as the emulated path does. The
  // shader adds this in clip space before the viewport maps it to pixels.
  if (REXCVAR_GET(svr_native_half_pixel_offset) && !(ReadReg(d3d, PA_SU_VTX_CNTL) & 1)) {
    const uint32_t vte = ReadReg(d3d, PA_CL_VTE_CNTL);
    const float x_scale = (vte & 1) ? ReadRegFloat(d3d, PA_CL_VPORT_XSCALE) : 1.0f;
    const float y_scale = (vte & 4) ? ReadRegFloat(d3d, PA_CL_VPORT_YSCALE) : 1.0f;
    shared->half_pixel_offset[0] = x_scale != 0.0f ? 0.5f / x_scale : 0.0f;
    shared->half_pixel_offset[1] = y_scale != 0.0f ? 0.5f / y_scale : 0.0f;
  }
  if (g.shared_address && REXCVAR_GET(svr_native_reuse_constants) &&
      std::memcmp(&g.last_shared, shared, sizeof(*shared)) == 0) {
    push.shared_constants = g.shared_address;
    return true;
  }
  const Upload upload = AllocateUpload(sizeof(SharedConstants), 16);
  if (!upload.data) {
    return false;
  }
  std::memcpy(upload.data, shared, sizeof(*shared));
  g.last_shared = *shared;
  g.shared_address = push.shared_constants = upload.address;
  return true;
}

}  // namespace

void Configure(rex::Runtime* runtime) {
  if (!REXCVAR_GET(svr_native_renderer)) {
    return;
  }
  auto* graphics_system = runtime ? runtime->graphics_system() : nullptr;
  // This build only has the Vulkan backend, so the provider is a VulkanProvider.
  auto* provider = graphics_system
                       ? static_cast<rex::ui::vulkan::VulkanProvider*>(graphics_system->provider())
                       : nullptr;
  g.device = provider ? provider->vulkan_device() : nullptr;
  bool ok = g.device && InitializeContext(g.device, runtime->memory()) &&
            CreateUploadRings(kFramesInFlight, kUploadRingSize) && textures::Initialize() &&
            geometry::Initialize() && write_watch::Initialize(runtime->memory()) &&
            shader_library::Initialize(g.device);
  for (FrameSlot& slot : g.slots) {
    ok = ok && CreateFrameSlot(slot);
  }
  if (!ok) {
    REXLOG_ERROR("native renderer: could not create Vulkan resources; using Xenos emulation");
    g.device = nullptr;
    return;
  }
  rex::system::external_frame::SetEnabled(true);
  REXLOG_INFO("native renderer: enabled, output {}x{}", g.width, g.height);
}

bool IsEnabled() { return g.device != nullptr; }

void OnSetRenderTarget(const uint8_t* base, uint32_t index, uint32_t surface) {
  if (!g.device || index >= 4 || !surface) {
    return;
  }
  const SurfaceSize size = DecodeSurfaceSize(LoadBE32(base + surface + kSurfaceSize));
  g.surface_width[index] = size.width;
  g.surface_height[index] = size.height;
}

void OnClear(const uint8_t* base, uint32_t device, uint32_t flags, const int32_t rect[4],
             uint32_t color, float depth, uint32_t stencil) {
  if (!g.device) {
    return;
  }
  BeginFrameIfNeeded();
  const uint8_t* d3d = base + device;
  const bool clear_color = (flags & 0xF) != 0;
  const bool clear_depth_stencil = (flags & 0x30) != 0;
  if (!EnsureRendering(d3d, clear_depth_stencil)) {
    return;
  }
  ++g.stats.clears;
  TRACE("clear flags {:X} rect {} {} {} {} colour {} depth {}", flags, rect[0], rect[1], rect[2],
        rect[3], color ? LoadBEFloat(base + color) : -1.0f, depth);
  VkClearAttachment attachments[2] = {};
  uint32_t count = 0;
  if (clear_color) {
    VkClearAttachment& a = attachments[count++];
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      a.clearValue.color.float32[i] = color ? LoadBEFloat(base + color + i * 4) : 0.0f;
    }
  }
  if (clear_depth_stencil && g.active_depth) {
    VkClearAttachment& a = attachments[count++];
    a.aspectMask = ((flags & 0x10) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                   ((flags & 0x20) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    a.clearValue.depthStencil = {depth, stencil & 0xFF};
  }
  if (!count) {
    return;
  }
  const RenderTarget& target = *g.active_color;
  VkClearRect clear_rect = {};
  clear_rect.layerCount = 1;
  const int32_t right = std::min<int32_t>(rect[2], int32_t(target.width));
  const int32_t bottom = std::min<int32_t>(rect[3], int32_t(target.height));
  if (right > rect[0] && bottom > rect[1] && rect[0] >= 0 && rect[1] >= 0) {
    clear_rect.rect = {{rect[0], rect[1]}, {uint32_t(right - rect[0]), uint32_t(bottom - rect[1])}};
  } else {
    clear_rect.rect = {{0, 0}, {target.width, target.height}};
  }
  g_vk.dfn->vkCmdClearAttachments(g.slot->main_cb, count, attachments, 1, &clear_rect);
}

namespace {

// Copies the current depth target's depth into the destination as R32 float,
// through a staging buffer since Vulkan cannot blit depth to colour.
void ResolveDepth(const uint8_t* d3d, const textures::FetchConstant& fetch) {
  const auto it = g.render_targets.find(
      uint64_t(ReadReg(d3d, RB_DEPTH_INFO) & 0x10FFF) |
      uint64_t(ReadReg(d3d, RB_SURFACE_INFO) & 0x3FFF) << 32 | 1ull << 63);
  if (it == g.render_targets.end()) {
    return;
  }
  RenderTarget& depth = *it->second;
  const textures::ResolveTarget target = textures::GetResolveTarget(fetch, false);
  if (!target.image || target.format != VK_FORMAT_R32_SFLOAT) {
    return;
  }
  const uint32_t width = std::min(depth.width, target.width);
  const uint32_t height = std::min(depth.height, target.height);
  const VkDeviceSize size = VkDeviceSize(depth.width) * depth.height * 4;
  if (g.depth_staging_size < size) {
    if (g.depth_staging) {
      // Frames still in flight may use the old buffer.
      Retired retired;
      retired.buffer = g.depth_staging;
      retired.memory = g.depth_staging_memory;
      Retire(std::move(retired));
      g.depth_staging = VK_NULL_HANDLE;
      g.depth_staging_memory = VK_NULL_HANDLE;
    }
    if (!vk_util::CreateDedicatedAllocationBuffer(
            g.device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vk_util::MemoryPurpose::kDeviceLocal, g.depth_staging, g.depth_staging_memory)) {
      g.depth_staging_size = 0;
      return;
    }
    g.depth_staging_size = size;
  }
  const VkCommandBuffer cb = g.slot->main_cb;
  const auto& dfn = *g_vk.dfn;
  TransitionTarget(depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.bufferRowLength = depth.width;
  region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  dfn.vkCmdCopyImageToBuffer(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             g.depth_staging, 1, &region);
  VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  buffer_barrier.buffer = g.depth_staging;
  buffer_barrier.size = VK_WHOLE_SIZE;
  dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 1, &buffer_barrier, 0, nullptr);
  ImageBarrier(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT, *target.layout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  dfn.vkCmdCopyBufferToImage(cb, g.depth_staging, target.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ImageBarrier(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  *target.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

}  // namespace

void OnResolve(const uint8_t* base, uint32_t device, uint32_t flags, uint32_t destination) {
  if (!g.device) {
    return;
  }
  BeginFrameIfNeeded();
  const uint8_t* d3d = base + device;
  const uint32_t copy_control = ReadReg(d3d, RB_COPY_CONTROL);
  const uint32_t dest_info = ReadReg(d3d, RB_COPY_DEST_INFO);
  const uint32_t source_select = copy_control & 7;
  const uint32_t copy_command = (copy_control >> 20) & 3;
  // Resolve flags 0..3 pick a colour target, 4 the depth-stencil surface
  // (copied into a 24_8 texture as float depth).
  const bool depth_resolve = (flags & 7) == 4;
  RenderTarget* source =
      source_select < 4 && !depth_resolve ? CurrentColorTarget(d3d, false) : nullptr;
  static uint32_t logged = 0;
  if (logged++ < 8) {
    REXLOG_INFO("native renderer: resolve dest={:08X} copy_control={:08X} dest_info={:08X} "
                "color_clear={:08X} source={}",
                destination, copy_control, dest_info, ReadReg(d3d, RB_COLOR_CLEAR),
                source ? "found" : "none");
  }
  ++g.stats.resolves;
  if (Tracing()) {
    const uint8_t* dest_fetch = base + destination + kTextureFetchConstant;
    TRACE("resolve dest {:08X} fetch {:08X} {:08X} {:08X} copy_control {:08X} dest_info {:08X} "
          "source {} {}x{}",
          destination, destination ? LoadBE32(dest_fetch) : 0,
          destination ? LoadBE32(dest_fetch + 4) : 0, destination ? LoadBE32(dest_fetch + 8) : 0,
          copy_control, dest_info, source ? "found" : "none", source ? source->width : 0,
          source ? source->height : 0);
  }
  EndRendering();
  const VkCommandBuffer cb = g.slot->main_cb;
  if (destination && depth_resolve) {
    ResolveDepth(d3d, textures::LoadFetchConstant(base + destination + kTextureFetchConstant));
  }
  if (destination && source && copy_command != 3) {
    const textures::FetchConstant fetch =
        textures::LoadFetchConstant(base + destination + kTextureFetchConstant);
    const textures::ResolveTarget target =
        textures::GetResolveTarget(fetch, (dest_info >> 24) & 1);
    if (target.image) {
      TransitionTarget(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      ImageBarrier(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT, *target.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      const int32_t width = int32_t(std::min(source->width, target.width));
      const int32_t height = int32_t(std::min(source->height, target.height));
      VkImageBlit blit = {};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {width, height, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[1] = {width, height, 1};
      g_vk.vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
      VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      QueueReadback(fetch, target, layout);
      ImageBarrier(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      *target.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  // Resolves can clear the source afterwards (RB_COPY_CONTROL bits 8 and 9).
  if ((copy_control >> 8) & 1) {
    if (RenderTarget* color = CurrentColorTarget(d3d, false)) {
      TransitionTarget(*color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      const uint32_t c = ReadReg(d3d, RB_COLOR_CLEAR);
      VkClearColorValue value;
      value.float32[0] = float((c >> 16) & 0xFF) / 255.0f;
      value.float32[1] = float((c >> 8) & 0xFF) / 255.0f;
      value.float32[2] = float(c & 0xFF) / 255.0f;
      value.float32[3] = float(c >> 24) / 255.0f;
      const VkImageSubresourceRange range = vk_util::InitializeSubresourceRange();
      g_vk.dfn->vkCmdClearColorImage(cb, color->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &value, 1, &range);
    }
  }
  if ((copy_control >> 9) & 1) {
    const auto it = g.render_targets.find(
        uint64_t(ReadReg(d3d, RB_DEPTH_INFO) & 0x10FFF) |
        uint64_t(ReadReg(d3d, RB_SURFACE_INFO) & 0x3FFF) << 32 | 1ull << 63);
    if (it != g.render_targets.end()) {
      RenderTarget& depth = *it->second;
      TransitionTarget(depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      const uint32_t d = ReadReg(d3d, RB_DEPTH_CLEAR);
      const VkClearDepthStencilValue value = {float(d >> 8) / float(0xFFFFFF), d & 0xFF};
      const VkImageSubresourceRange range = vk_util::InitializeSubresourceRange(
          VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      g_vk.vkCmdClearDepthStencilImage(cb, depth.image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                            &range);
    }
  }
}

namespace {

bool Topology(uint32_t primitive, VkPrimitiveTopology& topology, bool& quads) {
  quads = false;
  switch (primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case 13:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      return true;
    default:
      return false;
  }
}

// Shaders, declaration, render targets, pipeline, constants and dynamic
// state shared by both draw kinds. Leaves the pipeline bound; returns the
// declaration's layout, or nullptr when the draw is skipped.
const VertexLayout* PrepareDraw(const uint8_t* base, const uint8_t* d3d,
                                VkPrimitiveTopology topology,
                                const uint32_t strides[kMaxStreams]) {
  const NativeShader* vertex_shader =
      shader_library::Find(LoadBE32(d3d + kDeviceVertexShader));
  const NativeShader* pixel_shader = shader_library::Find(LoadBE32(d3d + kDevicePixelShader));
  if (!vertex_shader || !pixel_shader || vertex_shader->is_pixel_shader ||
      !pixel_shader->is_pixel_shader) {
    ++g.stats.no_shader;
    return nullptr;
  }
  const DeclEntry* declaration =
      GetDeclaration(base, LoadBE32(d3d + kDeviceVertexDeclaration));
  if (!declaration) {
    ++g.stats.no_declaration;
    return nullptr;
  }
  const VertexLayout& layout = declaration->layout;
  const uint32_t depth_control = ReadReg(d3d, RB_DEPTHCONTROL);
  if (!EnsureRendering(d3d, (depth_control & 7) != 0)) {
    ++g.stats.other;
    return nullptr;
  }

  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_shader = vertex_shader->module;
  key.pixel_shader = pixel_shader->module;
  const uint32_t color_control = ReadReg(d3d, RB_COLORCONTROL);
  uint32_t spec = 0;
  if (layout.packed_normal) {
    spec |= 1;  // SPEC_CONSTANT_R11G11B10_NORMAL
  }
  if ((color_control & 8) && (color_control & 7) != 7) {
    spec |= 2;  // SPEC_CONSTANT_ALPHA_TEST
  }
  key.spec_constants = spec & (vertex_shader->spec_constants_mask |
                               pixel_shader->spec_constants_mask);
  key.topology = topology;
  key.color_format = g.active_color->format;
  key.depth_format = g.active_depth ? g.active_depth->format : VK_FORMAT_UNDEFINED;
  key.blend_control = ReadReg(d3d, RB_BLENDCONTROL0);
  key.color_mask = ReadReg(d3d, RB_COLOR_MASK);
  key.depth_control = g.active_depth ? depth_control : 0;
  key.cull = ReadReg(d3d, PA_SU_SC_MODE_CNTL) & 7;
  for (uint32_t stream = 0; stream < kMaxStreams; ++stream) {
    key.strides[stream] = (layout.stream_mask & (1u << stream)) ? strides[stream] : 0;
  }
  key.dummy_locations = layout.dummy_locations;
  key.attribute_count = layout.attribute_count;
  std::memcpy(key.attributes, layout.attributes, sizeof(key.attributes));
  const VkPipeline pipeline = GetPipeline(key);
  if (!pipeline) {
    ++g.stats.other;
    return nullptr;
  }

  PushConstants push;
  const uint32_t fetch_slots = REXCVAR_GET(svr_native_bind_used_slots)
                                   ? vertex_shader->fetch_slots | pixel_shader->fetch_slots
                                   : UINT32_MAX;
  if (!UploadConstants(d3d, layout.swapped_texcoords, fetch_slots, push)) {
    ++g.stats.other;
    return nullptr;
  }
  const VkCommandBuffer cb = g.slot->main_cb;
  const auto& dfn = *g_vk.dfn;
  if (g.bound_pipeline != pipeline) {
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g.bound_pipeline = pipeline;
  }
  SetDynamicState(d3d, cb);
  if (std::memcmp(&push, &g.last_push, sizeof(push)) != 0) {
    dfn.vkCmdPushConstants(cb, g_vk.pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
    g.last_push = push;
  }
  if (layout.dummy_locations) {
    // Zeros for shader inputs the declaration does not feed, once per frame.
    if (!g.zeros.data) {
      g.zeros = AllocateUpload(64, 16);
      if (!g.zeros.data) {
        return nullptr;
      }
      std::memset(g.zeros.data, 0, 64);
    }
    dfn.vkCmdBindVertexBuffers(cb, kDummyBinding, 1, &g.zeros.buffer, &g.zeros.offset);
  }
  return &layout;
}

// Copies guest vertex data, which is fetched as 32-bit big-endian words.
bool UploadVertices(const uint8_t* source, uint32_t bytes, uint32_t binding) {
  const Upload upload = AllocateUpload((bytes + 3) & ~3u, 16);
  if (!upload.data) {
    return false;
  }
  SwapCopy32(reinterpret_cast<uint32_t*>(upload.data), source, (bytes + 3) / 4);
  g_vk.dfn->vkCmdBindVertexBuffers(g.slot->main_cb, binding, 1, &upload.buffer, &upload.offset);
  return true;
}

}  // namespace

void OnSetStreamSource(uint32_t stream, uint32_t stride) {
  if (stream < kMaxStreams) {
    g.stream_strides[stream] = stride;
  }
}

void OnDrawVertices(const uint8_t* base, uint32_t device, uint32_t primitive,
                    uint32_t vertex_count, uint32_t stride, uint32_t vertices) {
  if (!g.device || !vertex_count || !vertices || !stride || stride > 256 || (stride & 3)) {
    return;
  }
  BeginFrameIfNeeded();
  VkPrimitiveTopology topology;
  bool quads;
  if (!Topology(primitive, topology, quads)) {
    ++g.stats.unsupported_primitive;
    return;
  }
  const uint32_t strides[kMaxStreams] = {stride};
  const uint8_t* d3d = base + device;
  if (!PrepareDraw(base, d3d, topology, strides)) {
    return;
  }
  if (!UploadVertices(base + vertices, vertex_count * stride, 0)) {
    ++g.stats.other;
    return;
  }
  const VkCommandBuffer cb = g.slot->main_cb;
  const auto& dfn = *g_vk.dfn;
  if (quads) {
    const uint32_t quad_count = vertex_count / 4;
    const Upload index_upload = AllocateUpload(quad_count * 6 * sizeof(uint32_t), 16);
    if (!index_upload.data) {
      ++g.stats.other;
      return;
    }
    auto* indices = reinterpret_cast<uint32_t*>(index_upload.data);
    for (uint32_t q = 0; q < quad_count; ++q) {
      const uint32_t v = q * 4;
      const uint32_t quad[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
      std::memcpy(indices + q * 6, quad, sizeof(quad));
    }
    dfn.vkCmdBindIndexBuffer(cb, index_upload.buffer, index_upload.offset, VK_INDEX_TYPE_UINT32);
    dfn.vkCmdDrawIndexed(cb, quad_count * 6, 1, 0, 0, 0);
  } else {
    dfn.vkCmdDraw(cb, vertex_count, 1, 0, 0);
  }
  ++g.stats.drawn;
  TRACE("draw vertices prim {} count {} stride {} blend {:08X} depthctl {:08X}", primitive,
        vertex_count, stride, ReadReg(d3d, RB_BLENDCONTROL0), ReadReg(d3d, RB_DEPTHCONTROL));
}

void OnDrawIndexed(const uint8_t* base, uint32_t device, uint32_t primitive,
                   int32_t base_vertex, uint32_t start_index, uint32_t index_count) {
  if (!g.device || !index_count) {
    return;
  }
  BeginFrameIfNeeded();
  VkPrimitiveTopology topology;
  bool quads;
  if (!Topology(primitive, topology, quads)) {
    ++g.stats.unsupported_primitive;
    return;
  }
  const uint8_t* d3d = base + device;
  const uint32_t index_buffer = LoadBE32(d3d + kDeviceIndexBuffer);
  if (!index_buffer) {
    ++g.stats.other;
    return;
  }
  // Index buffer object: Common bit 31 = 32-bit indices, +0x18 = address.
  const bool index32 = (LoadBE32(base + index_buffer) & 0x80000000) != 0;
  const uint32_t index_address = (LoadBE32(base + index_buffer + 0x18) & ~3u) +
                                 start_index * (index32 ? 4 : 2);
  geometry::Indices indices;
  if (!geometry::IndexBuffer(ToPhysical(index_address), index_count, index32, quads,
                             g.slot->upload_cb, indices)) {
    return;
  }
  const uint32_t min_index = indices.min_index, max_index = indices.max_index;

  uint32_t strides[kMaxStreams];
  std::memcpy(strides, g.stream_strides, sizeof(strides));
  const VertexLayout* layout = PrepareDraw(base, d3d, topology, strides);
  if (!layout) {
    return;
  }
  // Vertex fetch constants written by SetStreamSource: stream s at device
  // +0x778 - 8 s (slot 95 - s), dword 0 = physical address | type 3.
  const uint32_t first_vertex = uint32_t(int32_t(min_index) + base_vertex);
  const uint32_t vertex_count = max_index - min_index + 1;
  for (uint32_t stream = 0; stream < kMaxStreams; ++stream) {
    if (!(layout->stream_mask & (1u << stream))) {
      continue;
    }
    const uint32_t fetch = LoadBE32(d3d + kDeviceStreamFetch - 8 * stream);
    const uint32_t stride = strides[stream];
    geometry::Region vertices;
    const bool ok = (fetch & 3) == 3 && stride &&
                    geometry::Vertices((fetch & ~3u) + first_vertex * stride,
                                       vertex_count * stride, g.slot->upload_cb, vertices);
    if (ok) {
      g_vk.dfn->vkCmdBindVertexBuffers(g.slot->main_cb, stream, 1, &vertices.buffer,
                                       &vertices.offset);
    }
    if (!ok) {
      ++g.stats.bad_stream;
      static uint32_t logged = 0;
      if (logged++ < 16) {
        REXLOG_WARN("native renderer: indexed draw stream {} fetch={:08X} {:08X} stride={} "
                    "indices {}..{} base_vertex={} count={} index32={}",
                    stream, fetch, LoadBE32(d3d + kDeviceStreamFetch - 8 * stream + 4), stride,
                    min_index, max_index, base_vertex, index_count, index32);
        std::string ib;
        for (uint32_t i = 0; i < 32; i += 4) {
          ib += fmt::format(" {:08X}", LoadBE32(base + index_buffer + i));
        }
        REXLOG_WARN("native renderer:   index buffer {:08X}:{} start={}", index_buffer, ib,
                    start_index);
      }
      return;
    }
  }
  const VkCommandBuffer cb = g.slot->main_cb;
  const auto& dfn = *g_vk.dfn;
  dfn.vkCmdBindIndexBuffer(cb, indices.region.buffer, indices.region.offset,
                           VK_INDEX_TYPE_UINT32);
  dfn.vkCmdDrawIndexed(cb, indices.count, 1, 0, 0, 0);
  ++g.stats.drawn_indexed;
  TRACE("draw indexed prim {} indices {} blend {:08X} depthctl {:08X}", primitive, index_count,
        ReadReg(d3d, RB_BLENDCONTROL0), ReadReg(d3d, RB_DEPTHCONTROL));
  if (Tracing()) {
    std::string c;
    for (uint32_t i = 0; i < 16; ++i) {
      c += fmt::format(" {:.3f}", LoadBEFloat(d3d + kDeviceVertexConstants + i * 4));
    }
    TRACE("  vte {:08X} vport {:.1f} {:.1f} {:.1f} {:.1f} {:.3f} {:.3f} mode {:08X} vs c0-3:{}",
          ReadReg(d3d, PA_CL_VTE_CNTL), ReadRegFloat(d3d, PA_CL_VPORT_XSCALE),
          ReadRegFloat(d3d, PA_CL_VPORT_XOFFSET), ReadRegFloat(d3d, PA_CL_VPORT_YSCALE),
          ReadRegFloat(d3d, PA_CL_VPORT_YOFFSET), ReadRegFloat(d3d, PA_CL_VPORT_ZSCALE),
          ReadRegFloat(d3d, PA_CL_VPORT_ZOFFSET), ReadReg(d3d, PA_SU_SC_MODE_CNTL), c);
    if (shader_library::Hash(LoadBE32(d3d + kDevicePixelShader)) == REXCVAR_GET(svr_native_trace_ps)) {
      for (uint32_t r = 0; r < 256; r += 4) {
        std::string c;
        for (uint32_t i = 0; i < 16; ++i) {
          c += fmt::format(" {:.4g}", LoadBEFloat(d3d + kDevicePixelConstants + r * 16 + i * 4));
        }
        TRACE("  ps c{}:{}", r, c);
      }
      std::string b;
      for (uint32_t i = 0; i < 8; ++i) {
        b += fmt::format(" {:08X}", LoadBE32(d3d + kDeviceBooleans + i * 4));
      }
      TRACE("  booleans:{} colorcontrol {:08X} colormask {:08X}", b, ReadReg(d3d, RB_COLORCONTROL),
            ReadReg(d3d, RB_COLOR_MASK));
    }
    TRACE("  vs {:016x} ps {:016x} first vertex {} verts {} streams {:X} fetch0 {:08X}",
          shader_library::Hash(LoadBE32(d3d + kDeviceVertexShader)),
          shader_library::Hash(LoadBE32(d3d + kDevicePixelShader)), first_vertex,
          vertex_count, layout->stream_mask, LoadBE32(d3d + kDeviceStreamFetch));
  }
}

void OnPresent(const uint8_t* base, uint32_t device, uint32_t front_buffer) {
  if (!g.device) {
    return;
  }
  BeginFrameIfNeeded();
  EndRendering();
  const VulkanDevice::Functions& dfn = *g_vk.dfn;
  FrameSlot& slot = *g.slot;
  const VkCommandBuffer cb = slot.main_cb;

  if (g.frame % 300 == 0) {
    const Stats& s = g.stats;
    REXLOG_INFO("native renderer: frame {}: drawn={} no_shader={} no_declaration={} "
                "unsupported_primitive={} drawn_indexed={} bad_stream={} other={} clears={} resolves={} "
                "pipelines={} render_targets={} readbacks={}",
                g.frame, s.drawn, s.no_shader, s.no_declaration, s.unsupported_primitive,
                s.drawn_indexed, s.bad_stream, s.other, s.clears, s.resolves, g.pipelines.size(),
                g.render_targets.size(), g.readbacks_delivered);
    g.stats = {};
  }

  ImageBarrier(cb, slot.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  textures::ResolveTarget source;
  if (front_buffer) {
    const textures::FetchConstant fetch =
        textures::LoadFetchConstant(base + front_buffer + kTextureFetchConstant);
    source = textures::FindResolveTarget(fetch.dwords[1] & 0xFFFFF000);
  }
  if (const uint32_t debug_resolve = REXCVAR_GET(svr_native_debug_resolve)) {
    const textures::ResolveTarget target = textures::FindResolveTarget(debug_resolve);
    if (target.image) {
      source = target;
    }
  }
  const int32_t debug_target = REXCVAR_GET(svr_native_debug_target);
  if (debug_target > 0 && size_t(debug_target) <= g.render_target_order.size()) {
    RenderTarget& target = *g.render_target_order[debug_target - 1];
    if (!target.is_depth) {
      TransitionTarget(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      source = {target.image, target.format, target.width, target.height, &target.layout};
    }
  }
  if (source.image) {
    ImageBarrier(cb, source.image, VK_IMAGE_ASPECT_COLOR_BIT, *source.layout,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(g.width), int32_t(g.height), 1};
    g_vk.vkCmdBlitImage(cb, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    ImageBarrier(cb, source.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    *source.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else {
    const VkClearColorValue black = {};
    const VkImageSubresourceRange range = vk_util::InitializeSubresourceRange();
    dfn.vkCmdClearColorImage(cb, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
  }
  // Later submissions on the same queue (the plugin's present) fall in the
  // barrier's second scope.
  ImageBarrier(cb, slot.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  geometry::EndFrame(slot.upload_cb);
  dfn.vkEndCommandBuffer(slot.upload_cb);
  dfn.vkEndCommandBuffer(slot.main_cb);

  const VkCommandBuffer command_buffers[2] = {slot.upload_cb, slot.main_cb};
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 2;
  submit_info.pCommandBuffers = command_buffers;
  g.recording = false;
  {
    VulkanDevice::Queue::Acquisition queue =
        g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, slot.fence) != VK_SUCCESS) {
      REXLOG_ERROR("native renderer: vkQueueSubmit failed");
      return;
    }
  }

  rex::system::external_frame::Frame frame;
  frame.image_view = uint64_t(reinterpret_cast<uintptr_t>(slot.view));
  frame.width = g.width;
  frame.height = g.height;
  frame.sequence = g.frame;
  rex::system::external_frame::Publish(frame);
  ++g.frame;
  PollTraceRequest();
}

}  // namespace svr::native
