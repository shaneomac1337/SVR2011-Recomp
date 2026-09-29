#include "native/shader_library.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <smolv.h>
#include <zstd.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include "native/shader_cache.h"

namespace svr::native::shader_library {

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct State {
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  std::vector<uint8_t> spirv_blob;  // smol-v streams, back to back
  std::unique_ptr<NativeShader[]> shaders;  // parallel to g_shaderCacheEntries
  std::mutex mutex;
  std::unordered_map<uint32_t, uint64_t> hash_by_object;
  uint32_t unknown_hashes = 0;
} g;

const ShaderCacheEntry* FindEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry* it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry& e, uint64_t h) { return e.hash < h; });
  return it != end && it->hash == hash ? it : nullptr;
}

bool BuildModule(const ShaderCacheEntry& entry, NativeShader& shader) {
  const uint8_t* encoded = g.spirv_blob.data() + entry.spirvOffset;
  const size_t decoded_size = smolv::GetDecodedBufferSize(encoded, entry.spirvSize);
  std::vector<uint32_t> spirv((decoded_size + 3) / 4);
  if (!decoded_size ||
      !smolv::Decode(encoded, entry.spirvSize, spirv.data(), decoded_size)) {
    return false;
  }
  shader.module =
      rex::ui::vulkan::util::CreateShaderModule(g.device, spirv.data(), decoded_size);
  shader.spec_constants_mask = entry.specConstantsMask;
  return shader.module != VK_NULL_HANDLE;
}

}  // namespace

bool Initialize(const rex::ui::vulkan::VulkanDevice* device) {
  if (!g_shaderCacheEntryCount) {
    REXLOG_ERROR("native renderer: this build has no converted shaders "
                 "(run scripts/convert-shaders.ps1 and rebuild)");
    return false;
  }
  g.spirv_blob.resize(g_spirvCacheDecompressedSize);
  const size_t size = ZSTD_decompress(g.spirv_blob.data(), g.spirv_blob.size(),
                                      g_compressedSpirvCache, g_spirvCacheCompressedSize);
  if (ZSTD_isError(size) || size != g_spirvCacheDecompressedSize) {
    REXLOG_ERROR("native renderer: could not decompress the shader cache");
    return false;
  }
  g.shaders = std::make_unique<NativeShader[]>(g_shaderCacheEntryCount);
  g.device = device;
  REXLOG_INFO("native renderer: {} converted shaders", g_shaderCacheEntryCount);
  return true;
}

void OnShaderCreated(const uint8_t* container, uint32_t guest_object, bool is_pixel_shader) {
  if (!g.device || !container || !guest_object) {
    return;
  }
  const size_t size = size_t(LoadBE32(container + 4)) + LoadBE32(container + 8);
  const uint64_t hash = XXH3_64bits(container, size);
  const ShaderCacheEntry* entry = FindEntry(hash);
  std::lock_guard lock(g.mutex);
  g.hash_by_object[guest_object] = hash;
  if (entry) {
    g.shaders[entry - g_shaderCacheEntries].is_pixel_shader = is_pixel_shader;
  } else if (g.unknown_hashes++ < 8) {
    REXLOG_WARN("native renderer: {} shader {:016X} is not in the converted cache",
                is_pixel_shader ? "pixel" : "vertex", hash);
  }
}

const NativeShader* Find(uint32_t guest_object) {
  std::lock_guard lock(g.mutex);
  const auto it = g.hash_by_object.find(guest_object);
  if (it == g.hash_by_object.end()) {
    return nullptr;
  }
  const ShaderCacheEntry* entry = FindEntry(it->second);
  if (!entry) {
    return nullptr;
  }
  NativeShader& shader = g.shaders[entry - g_shaderCacheEntries];
  if (!shader.module && !BuildModule(*entry, shader)) {
    REXLOG_ERROR("native renderer: could not build shader {:016X}", entry->hash);
    return nullptr;
  }
  return &shader;
}

uint64_t Hash(uint32_t guest_object) {
  std::lock_guard lock(g.mutex);
  const auto it = g.hash_by_object.find(guest_object);
  return it != g.hash_by_object.end() ? it->second : 0;
}

}  // namespace svr::native::shader_library
