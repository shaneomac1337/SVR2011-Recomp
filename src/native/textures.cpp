#include "native/textures.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "native/guest_decode.h"
#include "native/texture_layout.h"
#include "native/vk_context.h"
#include "native/write_watch.h"

REXCVAR_DEFINE_INT32(svr_native_anisotropy, 3, "SVR2011",
                     "Native renderer: anisotropic filtering for linear, mipmapped textures, as "
                     "anisotropic_override (-1 = the game's setting, 0 = off, 1..5 = 1x..16x)");
REXCVAR_DEFINE_BOOL(svr_native_texture_write_watch, true, "SVR2011",
                    "Native renderer: skip re-checking textures whose guest memory was not "
                    "written since the last upload (off: hash every texture every frame)");
REXCVAR_DEFINE_UINT32(svr_native_dump_texture, 0, "SVR2011",
                      "Native renderer debugging: write the texture at this physical address "
                      "to native_texture_<address>.dds when it is uploaded");

namespace svr::native::textures {

namespace {

// Writes decoded (host-order) blocks as a DDS for inspection; DXT1/3/5 and
// RGBA8 only.
void DumpDds(uint32_t address, VkFormat format, uint32_t width, uint32_t height,
             const uint8_t* data, size_t size) {
  uint32_t header[32] = {};
  header[0] = 0x20534444;  // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000;
  header[3] = height;
  header[4] = width;
  header[19] = 32;
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: header[20] = 0x4; header[21] = 0x31545844; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: header[20] = 0x4; header[21] = 0x33545844; break;
    case VK_FORMAT_BC3_UNORM_BLOCK: header[20] = 0x4; header[21] = 0x35545844; break;
    case VK_FORMAT_R8G8B8A8_UNORM:
      header[20] = 0x41;
      header[22] = 32;
      header[23] = 0xFF;
      header[24] = 0xFF00;
      header[25] = 0xFF0000;
      header[26] = 0xFF000000;
      break;
    default:
      return;
  }
  header[27] = 0x1000;
  const std::string path = fmt::format("native_texture_{:08X}.dds", address);
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(data, 1, size, file);
    std::fclose(file);
    REXLOG_INFO("native renderer: dumped texture to {}", path);
  }
}

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
namespace vk_util = rex::ui::vulkan::util;

using guest::ByteSwap32;
using guest::ToPhysical;
using texture_layout::EndianSwap;

struct FormatInfo {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t block_size = 1;  // texels per block side
  uint32_t bytes_per_block_log2 = 0;
};

// Host formats whose component order matches the guest one after the endian
// swap (component X in the lowest bits), so the fetch swizzle applies as is.
FormatInfo GetFormatInfo(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
      return {VK_FORMAT_R8G8B8A8_UNORM, 1, 2};
    case xenos::TextureFormat::k_DXT1:
      return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 3};
    case xenos::TextureFormat::k_DXT2_3:
      return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 4};
    case xenos::TextureFormat::k_DXT4_5:
      return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 4};
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return {VK_FORMAT_R8_UNORM, 1, 0};
    case xenos::TextureFormat::k_8_8:
      return {VK_FORMAT_R8G8_UNORM, 1, 1};
    case xenos::TextureFormat::k_5_6_5:
      return {VK_FORMAT_B5G6R5_UNORM_PACK16, 1, 1};
    case xenos::TextureFormat::k_2_10_10_10:
      return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 2};
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 3};
    case xenos::TextureFormat::k_16_16_FLOAT:
      return {VK_FORMAT_R16G16_SFLOAT, 1, 2};
    case xenos::TextureFormat::k_32_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, 1, 2};
    default:
      return {};
  }
}

struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 1;
  uint32_t layers = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Guest swizzle (12 bits) -> a view with that mapping and its bindless index.
  struct View {
    VkImageView view;
    DescriptorHeap heap;
    uint32_t index;
  };
  std::unordered_map<uint32_t, View> views;
  uint64_t content_hash = 0;
  // Guest memory the upload read: base level and mips (physical, bytes).
  uint32_t base_range[2] = {};
  uint32_t mips_range[2] = {};
  uint32_t watch_token = 0;
  // The last check could not upload: check again next frame.
  bool upload_failed = false;
  uint64_t checked_frame = UINT64_MAX;
  uint64_t last_used = 0;
  // Written in consecutive frames: not watched (hashed every frame) until
  // dynamic_until, so its writes cost no page faults.
  uint64_t dirty_frame = UINT64_MAX;
  uint32_t dirty_streak = 0;
  uint64_t dynamic_until = 0;
  bool uploaded = false;
  bool is_resolve_target = false;
  bool red_blue_swapped = false;
};

struct State {
  // Guest-memory textures by fetch constant (address, format, size, layout).
  std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures;
  // Resolve destinations by physical base address.
  std::unordered_map<uint32_t, std::unique_ptr<Texture>> resolve_targets;
  std::unordered_map<uint64_t, uint32_t> samplers;  // key -> bindless index
  Texture defaults[3];  // 2D, 3D, cube; bindless index 0 of each heap
  bool defaults_ready = false;
  uint32_t unsupported_logged = 0;
  uint32_t scale = 1;
  uint64_t upload_count = 0;
  uint64_t upload_bytes = 0;
  uint64_t upload_ns = 0;  // checks and uploads, hashing included
} g;

// Guest textures unused this many frames are destroyed (reloaded if needed).
constexpr uint64_t kEvictAfterFrames = 1800;
constexpr uint32_t kDynamicStreak = 3;
constexpr uint64_t kDynamicFrames = 600;

// Destroys the texture's image, memory and views once frames in flight are
// done with them, and frees its bindless slots.
void RetireTexture(Texture& texture) {
  Retired retired;
  retired.image = texture.image;
  retired.memory = texture.memory;
  for (const auto& [swizzle, view] : texture.views) {
    retired.views.push_back(view.view);
    retired.descriptors.emplace_back(view.heap, view.index);
  }
  Retire(std::move(retired));
  texture.image = VK_NULL_HANDLE;
  texture.memory = VK_NULL_HANDLE;
  texture.views.clear();
}

bool CreateImage(Texture& texture, VkImageType type, VkFormat format, uint32_t width,
                 uint32_t height, uint32_t depth, uint32_t layers, VkImageCreateFlags flags,
                 VkImageUsageFlags usage, uint32_t levels = 1) {
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = flags;
  image_info.imageType = type;
  image_info.format = format;
  image_info.extent = {width, height, depth};
  image_info.mipLevels = levels;
  image_info.arrayLayers = layers;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_util::CreateDedicatedAllocationImage(g_vk.device, image_info,
                                               vk_util::MemoryPurpose::kDeviceLocal,
                                               texture.image, texture.memory)) {
    return false;
  }
  texture.format = format;
  texture.width = width;
  texture.height = height;
  texture.levels = levels;
  texture.layers = layers;
  return true;
}

VkComponentSwizzle Swizzle(uint32_t selector, bool red_blue_swapped) {
  if (red_blue_swapped && (selector == 0 || selector == 2)) {
    selector ^= 2;
  }
  switch (selector) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

uint32_t ViewIndex(Texture& texture, uint32_t swizzle, DescriptorHeap heap,
                   VkImageViewType view_type) {
  const auto it = texture.views.find(swizzle);
  if (it != texture.views.end()) {
    return it->second.index;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = texture.image;
  view_info.viewType = view_type;
  view_info.format = texture.format;
  view_info.components.r = Swizzle(swizzle & 7, texture.red_blue_swapped);
  view_info.components.g = Swizzle((swizzle >> 3) & 7, texture.red_blue_swapped);
  view_info.components.b = Swizzle((swizzle >> 6) & 7, texture.red_blue_swapped);
  view_info.components.a = Swizzle((swizzle >> 9) & 7, texture.red_blue_swapped);
  view_info.subresourceRange = vk_util::InitializeSubresourceRange();
  if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) {
    view_info.subresourceRange.layerCount = 6;
  }
  VkImageView view;
  if (g_vk.dfn->vkCreateImageView(g_vk.vk_device, &view_info, nullptr, &view) != VK_SUCCESS) {
    return 0;
  }
  const uint32_t index = AllocateDescriptor(heap);
  if (index) {
    WriteImageDescriptor(heap, index, view);
  }
  texture.views.emplace(swizzle, Texture::View{view, heap, index});
  return index;
}

void Barrier(VkCommandBuffer cb, Texture& texture, VkImageLayout new_layout,
             VkPipelineStageFlags src_stage, VkAccessFlags src_access,
             VkPipelineStageFlags dst_stage, VkAccessFlags dst_access, uint32_t layers = 1) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = texture.layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.image;
  // All levels and layers.
  barrier.subresourceRange = vk_util::InitializeSubresourceRange();
  g_vk.dfn->vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
  texture.layout = new_layout;
}

void PrepareDefaults(VkCommandBuffer cb) {
  const uint32_t layer_counts[3] = {1, 1, 6};
  for (uint32_t i = 0; i < 3; ++i) {
    Texture& texture = g.defaults[i];
    Barrier(cb, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, layer_counts[i]);
    const VkClearColorValue transparent_black = {};
    VkImageSubresourceRange range = vk_util::InitializeSubresourceRange();
    range.layerCount = layer_counts[i];
    g_vk.dfn->vkCmdClearColorImage(cb, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &transparent_black, 1, &range);
    Barrier(cb, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT, layer_counts[i]);
  }
  g.defaults_ready = true;
}

VkFilter Filter(uint32_t filter) { return filter == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

// Maximum anisotropy for a sampler (1 = off). Follows the emulated path's
// anisotropic_override: svr_native_anisotropy applies to linear, mipmapped
// textures, otherwise the fetch constant's own setting is used; capped by the
// device.
float Anisotropy(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t min_level,
                 uint32_t max_level) {
  if (!g_vk.device->properties().samplerAnisotropy) {
    return 1.0f;
  }
  uint32_t aniso = uint32_t(fetch.aniso_filter);
  const int32_t override_level = REXCVAR_GET(svr_native_anisotropy);
  const uint32_t mip_filter = uint32_t(fetch.mip_filter);
  if (override_level >= 0 && override_level <= 5 && max_level > min_level &&
      fetch.mag_filter == xenos::TextureFilter::kLinear &&
      fetch.min_filter == xenos::TextureFilter::kLinear && mip_filter <= 1) {
    aniso = uint32_t(override_level);
  }
  if (aniso == 0 || aniso > 5) {
    return 1.0f;
  }
  return std::min(float(1u << (aniso - 1)), g_vk.device->properties().maxSamplerAnisotropy);
}

VkSamplerAddressMode AddressMode(uint32_t clamp) {
  switch (xenos::ClampMode(clamp)) {
    case xenos::ClampMode::kRepeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

uint32_t SamplerIndex(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t min_level = 0,
                      uint32_t max_level = 15) {
  const uint32_t fields = uint32_t(fetch.clamp_x) | uint32_t(fetch.clamp_y) << 3 |
                          uint32_t(fetch.clamp_z) << 6 | uint32_t(fetch.mag_filter) << 9 |
                          uint32_t(fetch.min_filter) << 11 | uint32_t(fetch.mip_filter) << 13 |
                          uint32_t(fetch.border_color) << 15 | min_level << 17 | max_level << 21;
  const float anisotropy = Anisotropy(fetch, min_level, max_level);
  const uint64_t key = uint64_t(fields) | uint64_t(anisotropy) << 32;
  const auto it = g.samplers.find(key);
  if (it != g.samplers.end()) {
    return it->second;
  }
  VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = Filter(uint32_t(fetch.mag_filter));
  sampler_info.minFilter = Filter(uint32_t(fetch.min_filter));
  sampler_info.mipmapMode = uint32_t(fetch.mip_filter) == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = AddressMode(uint32_t(fetch.clamp_x));
  sampler_info.addressModeV = AddressMode(uint32_t(fetch.clamp_y));
  sampler_info.addressModeW = AddressMode(uint32_t(fetch.clamp_z));
  // The image holds levels 0..max_level; kBaseMap samples the first only.
  sampler_info.minLod = float(min_level);
  sampler_info.maxLod = uint32_t(fetch.mip_filter) == 2 ? float(min_level) : float(max_level);
  if (anisotropy > 1.0f) {
    // As in the emulated path, anisotropic filtering is fully linear.
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = anisotropy;
  }
  switch (uint32_t(fetch.border_color)) {
    case 1: sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    case 2: sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    default: sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
  }
  VkSampler sampler;
  uint32_t index = 0;
  if (g_vk.dfn->vkCreateSampler(g_vk.vk_device, &sampler_info, nullptr, &sampler) ==
      VK_SUCCESS) {
    index = AllocateDescriptor(kHeapSampler);
    if (index) {
      WriteSamplerDescriptor(index, sampler);
    }
  }
  g.samplers.emplace(key, index);
  return index;
}

// Guest texture: 2D or cube, all mip levels (base from base_page, the rest
// from mip_page, with the packed mip tail) per the SDK's guest layout.
struct GuestTexture {
  xenos::xe_gpu_texture_fetch_t fetch;
  FormatInfo info;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t base_page;
  uint32_t mip_page;
  uint32_t min_level;
  uint32_t max_level;
};

// Uploads every level from guest memory; false when the data is unchanged
// since the last upload.
bool UploadTexture(Texture& texture, const GuestTexture& t, VkCommandBuffer cb, bool watch) {
  const xenos::xe_gpu_texture_fetch_t& fetch = t.fetch;
  const uint32_t block = t.info.block_size;
  const uint32_t bpb_log2 = t.info.bytes_per_block_log2;
  const uint32_t bytes_per_block = 1u << bpb_log2;
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      fetch.dimension, uint32_t(fetch.pitch), t.width, t.height, t.layers, fetch.tiled,
      fetch.format, fetch.packed_mips, t.base_page != 0, t.max_level);
  const uint8_t* base = t.base_page ? TranslatePhysical(t.base_page << 12) : nullptr;
  const uint8_t* mips = t.mip_page ? TranslatePhysical(t.mip_page << 12) : nullptr;
  texture.base_range[0] = t.base_page << 12;
  texture.base_range[1] = base ? layout.base.level_data_extent_bytes : 0;
  texture.mips_range[0] = t.mip_page << 12;
  texture.mips_range[1] = mips && t.max_level ? layout.mips_total_extent_bytes : 0;
  uint32_t watch_token = 0;
  if (watch) {
    // Before reading: a write after this makes the texture dirty again.
    watch_token = std::min(write_watch::Watch(texture.base_range[0], texture.base_range[1]),
                           write_watch::Watch(texture.mips_range[0], texture.mips_range[1]));
  }
  uint64_t hash = 0;
  if (base) {
    hash = XXH3_64bits(base, layout.base.level_data_extent_bytes);
  }
  if (mips && t.max_level) {
    hash ^= XXH3_64bits(mips, layout.mips_total_extent_bytes) * 31;
  }
  texture.watch_token = watch_token;
  texture.upload_failed = false;
  if (texture.uploaded && hash == texture.content_hash) {
    return false;
  }

  // Host copy: level by level, each layer tightly packed.
  size_t total = 0;
  for (uint32_t level = 0; level <= t.max_level; ++level) {
    const uint32_t bx = (std::max(t.width >> level, 1u) + block - 1) / block;
    const uint32_t by = (std::max(t.height >> level, 1u) + block - 1) / block;
    total += size_t(bx) * by * bytes_per_block * t.layers;
  }
  const Upload upload = AllocateUpload(total, 16);
  if (!upload.data) {
    texture.upload_failed = true;
    return false;
  }
  texture.content_hash = hash;
  texture.uploaded = true;

  VkBufferImageCopy regions[16] = {};
  uint32_t region_count = 0;
  size_t offset = 0;
  for (uint32_t level = 0; level <= t.max_level; ++level) {
    const uint32_t level_width = std::max(t.width >> level, 1u);
    const uint32_t level_height = std::max(t.height >> level, 1u);
    const uint32_t bx = (level_width + block - 1) / block;
    const uint32_t by = (level_height + block - 1) / block;
    const size_t row_bytes = size_t(bx) * bytes_per_block;
    // Where the level is stored: the base, a mip, or inside the packed tail.
    const bool from_base = level == 0;
    const uint8_t* source = from_base ? base : mips;
    const uint32_t stored_level = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& stored =
        from_base ? layout.base : layout.mips[stored_level];
    if (!from_base) {
      source = mips ? mips + layout.mip_offsets_bytes[stored_level] : nullptr;
    }
    uint32_t tail_x = 0, tail_y = 0, tail_z = 0;
    if (level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(t.width, t.height, 1, fetch.format, level, tail_x, tail_y,
                                       tail_z);
    }
    for (uint32_t layer = 0; layer < t.layers; ++layer) {
      uint8_t* dest = upload.data + offset + size_t(layer) * row_bytes * by;
      if (!source) {
        std::memset(dest, 0, row_bytes * by);
        continue;
      }
      texture_layout::CopyLevelFromGuest(
          dest, source + size_t(layer) * stored.array_slice_stride_bytes, bx, by, bpb_log2,
          stored.row_pitch_bytes, fetch.tiled, tail_x, tail_y);
    }
    VkBufferImageCopy& region = regions[region_count++];
    region.bufferOffset = upload.offset + offset;
    region.bufferRowLength = bx * block;
    region.bufferImageHeight = by * block;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t.layers};
    region.imageExtent = {level_width, level_height, 1};
    offset += row_bytes * by * t.layers;
  }
  EndianSwap(upload.data, total, fetch.endianness);
  if (const uint32_t dump = REXCVAR_GET(svr_native_dump_texture);
      dump && dump == t.base_page << 12) {
    DumpDds(dump, t.info.format, t.width, t.height, upload.data,
            size_t((t.width + block - 1) / block) * ((t.height + block - 1) / block) *
                bytes_per_block);
  }

  Barrier(cb, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  g_vk.dfn->vkCmdCopyBufferToImage(cb, upload.buffer, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count, regions);
  Barrier(cb, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT);
  return true;
}

xenos::xe_gpu_texture_fetch_t Decode(const FetchConstant& fetch) {
  xenos::xe_gpu_texture_fetch_t decoded;
  std::memcpy(&decoded, fetch.dwords, sizeof(decoded));
  return decoded;
}

}  // namespace

FetchConstant LoadFetchConstant(const uint8_t* big_endian) {
  FetchConstant fetch;
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t v;
    std::memcpy(&v, big_endian + i * 4, 4);
    fetch.dwords[i] = ByteSwap32(v);
  }
  return fetch;
}

bool Initialize(uint32_t scale) {
  g.scale = scale;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!CreateImage(g.defaults[0], VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0,
                   usage) ||
      !CreateImage(g.defaults[1], VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0,
                   usage) ||
      !CreateImage(g.defaults[2], VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6,
                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, usage)) {
    return false;
  }
  // Index 0 of every heap.
  ViewIndex(g.defaults[0], 0x688, kHeapTexture2D, VK_IMAGE_VIEW_TYPE_2D);
  ViewIndex(g.defaults[1], 0x688, kHeapTexture3D, VK_IMAGE_VIEW_TYPE_3D);
  ViewIndex(g.defaults[2], 0x688, kHeapTextureCube, VK_IMAGE_VIEW_TYPE_CUBE);
  xenos::xe_gpu_texture_fetch_t linear_clamp = {};
  linear_clamp.clamp_x = linear_clamp.clamp_y = linear_clamp.clamp_z =
      xenos::ClampMode::kClampToEdge;
  linear_clamp.mag_filter = linear_clamp.min_filter = xenos::TextureFilter::kLinear;
  SamplerIndex(linear_clamp);
  return true;
}

Binding Bind(const FetchConstant& raw, VkCommandBuffer upload_cb, uint64_t frame) {
  if (!g.defaults_ready) {
    PrepareDefaults(upload_cb);
  }
  GuestTexture t;
  t.fetch = Decode(raw);
  const xenos::xe_gpu_texture_fetch_t& fetch = t.fetch;
  Binding binding;
  if (uint32_t(fetch.type) != 2) {
    return binding;
  }
  binding.dimension = uint32_t(fetch.dimension);
  const bool cube = fetch.dimension == xenos::DataDimension::kCube;
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked && !cube) {
    // No traced scene uses 1D or 3D textures; say so if one ever does.
    if (g.unsupported_logged++ < 16) {
      REXLOG_WARN("native renderer: {}D texture at {:08X} not supported yet",
                  fetch.dimension == xenos::DataDimension::k1D ? 1 : 3,
                  ToPhysical(uint32_t(fetch.base_address) << 12));
    }
    binding.sampler_index = SamplerIndex(fetch);
    return binding;
  }
  const uint32_t base_address = ToPhysical(uint32_t(fetch.base_address) << 12);
  const uint32_t swizzle = uint32_t(fetch.swizzle);

  if (!cube) {
    const auto resolved = g.resolve_targets.find(base_address);
    if (resolved != g.resolve_targets.end()) {
      binding.sampler_index = SamplerIndex(fetch, 0, 0);
      binding.texture_index =
          ViewIndex(*resolved->second, swizzle, kHeapTexture2D, VK_IMAGE_VIEW_TYPE_2D);
      return binding;
    }
  }

  t.info = GetFormatInfo(fetch.format);
  if (t.info.format == VK_FORMAT_UNDEFINED) {
    binding.sampler_index = SamplerIndex(fetch);
    if (g.unsupported_logged++ < 16) {
      // 24_8 textures exist only as depth resolve targets; one that is not
      // means the resolve that should have written it found no depth target.
      REXLOG_WARN("native renderer: texture format {} at {:08X} not supported yet",
                  uint32_t(fetch.format), base_address);
    }
    return binding;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &t.base_page, &t.mip_page,
                                                 &t.min_level, &t.max_level);
  t.width = width_minus_1 + 1;
  t.height = height_minus_1 + 1;
  // Stacked 2D arrays use their first layer for now (no traced scene has one).
  t.layers = cube ? 6 : 1;
  if (!cube && depth_minus_1 && g.unsupported_logged++ < 16) {
    REXLOG_WARN("native renderer: stacked texture at {:08X} ({} layers) draws its first layer",
                base_address, depth_minus_1 + 1);
  }
  if (t.base_page) {
    t.base_page = ToPhysical(t.base_page << 12) >> 12;
  }
  if (t.mip_page) {
    t.mip_page = ToPhysical(t.mip_page << 12) >> 12;
  }
  binding.sampler_index = SamplerIndex(fetch, t.min_level, t.max_level);
  if (!t.base_page && !t.mip_page) {
    return binding;
  }

  const uint64_t key =
      XXH3_64bits(&fetch, sizeof(fetch.dword_0) * 3) ^
      (uint64_t(t.mip_page) << 40 | uint64_t(t.max_level) << 32 | uint64_t(fetch.packed_mips) << 36);
  std::unique_ptr<Texture>& slot = g.textures[key];
  if (!slot) {
    slot = std::make_unique<Texture>();
    if (!CreateImage(*slot, VK_IMAGE_TYPE_2D, t.info.format, t.width, t.height, 1, t.layers,
                     cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     t.max_level + 1)) {
      REXLOG_ERROR("native renderer: could not create a {}x{} texture", t.width, t.height);
      g.textures.erase(key);
      return binding;
    }
  }
  Texture& texture = *slot;
  texture.last_used = frame;
  if (texture.checked_frame != frame) {
    texture.checked_frame = frame;
    const bool watch =
        REXCVAR_GET(svr_native_texture_write_watch) && frame >= texture.dynamic_until;
    const bool clean =
        watch && texture.uploaded && !texture.upload_failed &&
        (!texture.base_range[1] ||
         !write_watch::IsDirty(texture.base_range[0], texture.base_range[1],
                               texture.watch_token)) &&
        (!texture.mips_range[1] ||
         !write_watch::IsDirty(texture.mips_range[0], texture.mips_range[1],
                               texture.watch_token));
    if (!clean) {
      if (watch && texture.uploaded) {
        texture.dirty_streak = texture.dirty_frame + 1 == frame ? texture.dirty_streak + 1 : 1;
        texture.dirty_frame = frame;
        if (texture.dirty_streak >= kDynamicStreak) {
          texture.dynamic_until = frame + kDynamicFrames;
          texture.dirty_streak = 0;
        }
      }
      const auto start = std::chrono::steady_clock::now();
      if (UploadTexture(texture, t, upload_cb, watch && frame >= texture.dynamic_until)) {
        ++g.upload_count;
        g.upload_bytes += uint64_t(t.width) * t.height * t.layers
                          << t.info.bytes_per_block_log2 >> (t.info.block_size == 4 ? 4 : 0);
      }
      g.upload_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count());
    }
  }
  if (!texture.uploaded) {
    return binding;
  }
  binding.texture_index =
      cube ? ViewIndex(texture, swizzle, kHeapTextureCube, VK_IMAGE_VIEW_TYPE_CUBE)
           : ViewIndex(texture, swizzle, kHeapTexture2D, VK_IMAGE_VIEW_TYPE_2D);
  return binding;
}

ResolveTarget GetResolveTarget(const FetchConstant& raw, bool red_blue_swapped) {
  const xenos::xe_gpu_texture_fetch_t fetch = Decode(raw);
  const uint32_t base_address = ToPhysical(uint32_t(fetch.base_address) << 12);
  const uint32_t width = (fetch.size_2d.width + 1) * g.scale;
  const uint32_t height = (fetch.size_2d.height + 1) * g.scale;
  FormatInfo info = GetFormatInfo(fetch.format);
  if (fetch.format == xenos::TextureFormat::k_24_8 ||
      fetch.format == xenos::TextureFormat::k_24_8_FLOAT) {
    // Depth resolves: the depth value as a float in X.
    info.format = VK_FORMAT_R32_SFLOAT;
  } else if (info.format == VK_FORMAT_UNDEFINED || info.block_size != 1) {
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
  }
  std::unique_ptr<Texture>& slot = g.resolve_targets[base_address];
  if (slot && (slot->width != width || slot->height != height || slot->format != info.format)) {
    RetireTexture(*slot);
    slot.reset();
  }
  if (!slot) {
    slot = std::make_unique<Texture>();
    slot->is_resolve_target = true;
    if (!CreateImage(*slot, VK_IMAGE_TYPE_2D, info.format, width, height, 1, 1, 0,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      slot.reset();
      return {};
    }
  }
  if (slot->red_blue_swapped != red_blue_swapped) {
    slot->red_blue_swapped = red_blue_swapped;
    Retired retired;
    for (const auto& [swizzle, view] : slot->views) {
      retired.views.push_back(view.view);
      retired.descriptors.emplace_back(view.heap, view.index);
    }
    Retire(std::move(retired));
    slot->views.clear();
  }
  return {slot->image, slot->format, slot->width, slot->height, &slot->layout};
}

uint32_t GuestBytesPerPixel(const FetchConstant& raw) {
  const FormatInfo info = GetFormatInfo(Decode(raw).format);
  return info.format != VK_FORMAT_UNDEFINED && info.block_size == 1
             ? 1u << info.bytes_per_block_log2
             : 0;
}

bool BytewiseUnorm(const FetchConstant& raw) {
  switch (Decode(raw).format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_8_8:
      return true;
    default:
      return false;
  }
}

void WriteToGuest(const FetchConstant& raw, const uint8_t* pixels, uint32_t width,
                  uint32_t height) {
  const xenos::xe_gpu_texture_fetch_t fetch = Decode(raw);
  const FormatInfo info = GetFormatInfo(fetch.format);
  if (info.format == VK_FORMAT_UNDEFINED || info.block_size != 1) {
    return;
  }
  const uint32_t bpp = 1u << info.bytes_per_block_log2;
  const uint32_t pitch = uint32_t(fetch.pitch) << 5;
  const uint32_t linear_row_bytes = texture_layout::LinearRowBytes(pitch, info.bytes_per_block_log2);
  const uint32_t physical = ToPhysical(uint32_t(fetch.base_address) << 12);
  auto* dest = const_cast<uint8_t*>(TranslatePhysical(physical));
  // These writes bypass the guest's page protection; cached copies of the
  // pages must still see them.
  // Tiled levels span whole 32x32 tiles; pitch is a multiple of 32.
  write_watch::MarkWritten(physical, fetch.tiled ? ((height + 31) & ~31u) * pitch * bpp
                                                 : linear_row_bytes * height);
  std::vector<uint8_t> row(size_t(width) * bpp);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(row.data(), pixels + size_t(y) * width * bpp, row.size());
    // The same swap converts host order to guest order and back.
    EndianSwap(row.data(), row.size(), fetch.endianness);
    if (fetch.tiled) {
      for (uint32_t x = 0; x < width; ++x) {
        std::memcpy(dest + texture_layout::GuestOffset(x, y, pitch, info.bytes_per_block_log2,
                                                       true, linear_row_bytes),
                    row.data() + size_t(x) * bpp, bpp);
      }
    } else {
      std::memcpy(dest + size_t(y) * linear_row_bytes, row.data(), row.size());
    }
  }
}

void TakeUploadStats(uint64_t& count, uint64_t& bytes, uint64_t& ns) {
  count = g.upload_count;
  bytes = g.upload_bytes;
  ns = g.upload_ns;
  g.upload_count = g.upload_bytes = g.upload_ns = 0;
}

void EvictUnused(uint64_t frame) {
  for (auto it = g.textures.begin(); it != g.textures.end();) {
    if (it->second->last_used + kEvictAfterFrames < frame) {
      RetireTexture(*it->second);
      it = g.textures.erase(it);
    } else {
      ++it;
    }
  }
}

ResolveTarget FindResolveTarget(uint32_t base_address) {
  const auto it = g.resolve_targets.find(ToPhysical(base_address));
  if (it == g.resolve_targets.end()) {
    return {};
  }
  Texture& texture = *it->second;
  return {texture.image, texture.format, texture.width, texture.height, &texture.layout};
}

}  // namespace svr::native::textures
