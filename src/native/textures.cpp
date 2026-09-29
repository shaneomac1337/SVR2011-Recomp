#include "native/textures.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "native/vk_context.h"

namespace svr::native::textures {

namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
namespace vk_util = rex::ui::vulkan::util;

uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

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
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Guest swizzle (12 bits) -> bindless index of a view with that mapping.
  std::unordered_map<uint32_t, uint32_t> views;
  uint64_t content_hash = 0;
  uint64_t checked_frame = UINT64_MAX;
  bool uploaded = false;
  bool is_resolve_target = false;
  bool red_blue_swapped = false;
};

struct State {
  // Guest-memory textures by fetch constant (address, format, size, layout).
  std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures;
  // Resolve destinations by physical base address.
  std::unordered_map<uint32_t, std::unique_ptr<Texture>> resolve_targets;
  std::unordered_map<uint32_t, uint32_t> samplers;  // key -> bindless index
  Texture defaults[3];  // 2D, 3D, cube; bindless index 0 of each heap
  bool defaults_ready = false;
  uint32_t unsupported_logged = 0;
} g;

bool CreateImage(Texture& texture, VkImageType type, VkFormat format, uint32_t width,
                 uint32_t height, uint32_t depth, uint32_t layers, VkImageCreateFlags flags,
                 VkImageUsageFlags usage) {
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = flags;
  image_info.imageType = type;
  image_info.format = format;
  image_info.extent = {width, height, depth};
  image_info.mipLevels = 1;
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
    return it->second;
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
  texture.views.emplace(swizzle, index);
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
  barrier.subresourceRange = vk_util::InitializeSubresourceRange();
  barrier.subresourceRange.layerCount = layers;
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

uint32_t SamplerIndex(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t key = uint32_t(fetch.clamp_x) | uint32_t(fetch.clamp_y) << 3 |
                       uint32_t(fetch.clamp_z) << 6 | uint32_t(fetch.mag_filter) << 9 |
                       uint32_t(fetch.min_filter) << 11 | uint32_t(fetch.mip_filter) << 13 |
                       uint32_t(fetch.border_color) << 15;
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
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
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

void EndianSwap(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = ByteSwap32(v);
        std::memcpy(data + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

// Uploads mip 0 of a 2D texture from guest memory; false when the data is
// unchanged since the last upload.
bool UploadTexture(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch,
            const FormatInfo& info, VkCommandBuffer cb) {
  const uint32_t block = info.block_size;
  const uint32_t bytes_per_block = 1u << info.bytes_per_block_log2;
  const uint32_t blocks_x = (texture.width + block - 1) / block;
  const uint32_t blocks_y = (texture.height + block - 1) / block;
  const uint32_t pitch_blocks = (uint32_t(fetch.pitch) << 5) / block;
  const uint32_t linear_row_bytes =
      (pitch_blocks * bytes_per_block + xenos::kTextureLinearRowAlignmentBytes - 1) &
      ~(xenos::kTextureLinearRowAlignmentBytes - 1);
  const uint32_t source_size =
      fetch.tiled ? texture_util::GetTiledAddressUpperBound2D(blocks_x, blocks_y, pitch_blocks,
                                                              info.bytes_per_block_log2)
                  : linear_row_bytes * blocks_y;
  const uint8_t* source = TranslatePhysical(uint32_t(fetch.base_address) << 12);
  const uint64_t hash = XXH3_64bits(source, source_size);
  if (texture.uploaded && hash == texture.content_hash) {
    return false;
  }
  const uint32_t row_bytes = blocks_x * bytes_per_block;
  const Upload upload = AllocateUpload(VkDeviceSize(row_bytes) * blocks_y, 16);
  if (!upload.data) {
    return false;
  }
  texture.content_hash = hash;
  texture.uploaded = true;

  for (uint32_t y = 0; y < blocks_y; ++y) {
    uint8_t* row = upload.data + size_t(y) * row_bytes;
    if (fetch.tiled) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const int32_t offset = texture_util::GetTiledOffset2D(int32_t(x), int32_t(y),
                                                             pitch_blocks,
                                                             info.bytes_per_block_log2);
        std::memcpy(row + x * bytes_per_block, source + offset, bytes_per_block);
      }
    } else {
      std::memcpy(row, source + size_t(y) * linear_row_bytes, row_bytes);
    }
  }
  EndianSwap(upload.data, size_t(row_bytes) * blocks_y, fetch.endianness);

  Barrier(cb, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy region = {};
  region.bufferOffset = upload.offset;
  region.bufferRowLength = blocks_x * block;
  region.bufferImageHeight = blocks_y * block;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {texture.width, texture.height, 1};
  g_vk.dfn->vkCmdCopyBufferToImage(cb, upload.buffer, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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

bool Initialize() {
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
  const xenos::xe_gpu_texture_fetch_t fetch = Decode(raw);
  Binding binding;
  if (uint32_t(fetch.type) != 2) {
    return binding;
  }
  binding.dimension = uint32_t(fetch.dimension);
  binding.sampler_index = SamplerIndex(fetch);
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    return binding;
  }
  const uint32_t base_address = uint32_t(fetch.base_address) << 12;
  const uint32_t swizzle = uint32_t(fetch.swizzle);

  const auto resolved = g.resolve_targets.find(base_address);
  if (resolved != g.resolve_targets.end()) {
    binding.texture_index =
        ViewIndex(*resolved->second, swizzle, kHeapTexture2D, VK_IMAGE_VIEW_TYPE_2D);
    return binding;
  }

  const FormatInfo info = GetFormatInfo(fetch.format);
  if (info.format == VK_FORMAT_UNDEFINED) {
    if (g.unsupported_logged++ < 16) {
      REXLOG_WARN("native renderer: texture format {} not supported yet",
                  uint32_t(fetch.format));
    }
    return binding;
  }
  const uint32_t width = fetch.size_2d.width + 1;
  const uint32_t height = fetch.size_2d.height + 1;
  const uint64_t key = uint64_t(base_address) | uint64_t(fetch.format) << 32 |
                       uint64_t(fetch.tiled) << 38 | uint64_t(fetch.pitch) << 39 |
                       uint64_t(fetch.endianness) << 48 |
                       uint64_t(XXH3_64bits(&fetch.dword_2, 4) & 0x3FFF) << 50;
  std::unique_ptr<Texture>& slot = g.textures[key];
  if (!slot) {
    slot = std::make_unique<Texture>();
    if (!CreateImage(*slot, VK_IMAGE_TYPE_2D, info.format, width, height, 1, 1, 0,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      REXLOG_ERROR("native renderer: could not create a {}x{} texture", width, height);
      return binding;
    }
  }
  Texture& texture = *slot;
  if (texture.checked_frame != frame) {
    texture.checked_frame = frame;
    UploadTexture(texture, fetch, info, upload_cb);
  }
  if (!texture.uploaded) {
    return binding;
  }
  binding.texture_index = ViewIndex(texture, swizzle, kHeapTexture2D, VK_IMAGE_VIEW_TYPE_2D);
  return binding;
}

ResolveTarget GetResolveTarget(const FetchConstant& raw, bool red_blue_swapped) {
  const xenos::xe_gpu_texture_fetch_t fetch = Decode(raw);
  const uint32_t base_address = uint32_t(fetch.base_address) << 12;
  const uint32_t width = fetch.size_2d.width + 1;
  const uint32_t height = fetch.size_2d.height + 1;
  FormatInfo info = GetFormatInfo(fetch.format);
  if (info.format == VK_FORMAT_UNDEFINED || info.block_size != 1) {
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
  }
  std::unique_ptr<Texture>& slot = g.resolve_targets[base_address];
  if (slot && (slot->width != width || slot->height != height || slot->format != info.format)) {
    // Views stay allocated; the heap is large and this is rare.
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
    slot->views.clear();
  }
  return {slot->image, slot->format, slot->width, slot->height, &slot->layout};
}

ResolveTarget FindResolveTarget(uint32_t base_address) {
  const auto it = g.resolve_targets.find(base_address);
  if (it == g.resolve_targets.end()) {
    return {};
  }
  Texture& texture = *it->second;
  return {texture.image, texture.format, texture.width, texture.height, &texture.layout};
}

}  // namespace svr::native::textures
