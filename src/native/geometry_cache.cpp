#include "native/geometry_cache.h"

#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "native/guest_decode.h"
#include "native/vk_context.h"
#include "native/write_watch.h"

REXCVAR_DEFINE_BOOL(svr_native_geometry_cache, true, "SVR2011",
                    "Native renderer: keep vertex and index data the game does not rewrite in "
                    "GPU memory instead of uploading it for every draw");

namespace svr::native::geometry {

namespace {

namespace vk_util = rex::ui::vulkan::util;

constexpr VkDeviceSize kArenaSize = 128ull << 20;
// Rewritten in this many consecutive frames: the buffer is dynamic, and goes
// through the upload ring (unwatched) for kDynamicFrames.
constexpr uint32_t kDynamicStreak = 3;
constexpr uint64_t kDynamicFrames = 600;
constexpr uint64_t kPruneAfterFrames = 1800;

struct Entry {
  VkDeviceSize offset = 0;
  uint32_t generation = UINT32_MAX;  // the arena it lives in
  uint32_t watch_token = 0;
  uint64_t uploaded_frame = UINT64_MAX;
  uint64_t last_used = 0;
  uint64_t dynamic_until = 0;
  uint32_t streak = 0;
  // Index buffers only.
  uint32_t count = 0;
  uint32_t min_index = 0;
  uint32_t max_index = 0;
};

struct State {
  VkBuffer arena = VK_NULL_HANDLE;
  VkDeviceMemory arena_memory = VK_NULL_HANDLE;
  VkDeviceSize arena_used = 0;
  uint32_t generation = 0;
  bool copied = false;
  uint64_t pruned_frame = 0;
  std::unordered_map<uint64_t, Entry> vertices;
  std::unordered_map<uint64_t, Entry> indices;
  std::vector<uint32_t> index_scratch;
} g;

bool CreateArena() {
  return vk_util::CreateDedicatedAllocationBuffer(
      g_vk.device, kArenaSize,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      vk_util::MemoryPurpose::kDeviceLocal, g.arena, g.arena_memory);
}

// Space in the arena; a full arena is replaced (retired once frames in
// flight are done with it), which invalidates every entry.
bool AllocateArena(VkDeviceSize size, VkDeviceSize& offset) {
  if (size > kArenaSize / 4) {
    return false;
  }
  offset = (g.arena_used + 15) & ~VkDeviceSize(15);
  if (offset + size > kArenaSize) {
    Retired retired;
    retired.buffer = g.arena;
    retired.memory = g.arena_memory;
    Retire(std::move(retired));
    g.arena = VK_NULL_HANDLE;
    g.arena_memory = VK_NULL_HANDLE;
    if (!CreateArena()) {
      REXLOG_ERROR("native renderer: could not replace the geometry arena");
      return false;
    }
    ++g.generation;
    offset = 0;
    REXLOG_INFO("native renderer: geometry arena full, starting generation {}", g.generation);
  }
  g.arena_used = offset + size;
  return true;
}

void Prune(std::unordered_map<uint64_t, Entry>& map, uint64_t frame) {
  std::erase_if(map, [frame](const auto& item) {
    return item.second.last_used + kPruneAfterFrames < frame;
  });
}

// Where host_bytes of converted data for guest range [physical, +guest_bytes)
// go. Returns nullptr when the entry's copy is still valid (reuse), else the
// memory to fill, read from guest memory only after this call. out receives
// the region draws bind either way.
uint8_t* Place(Entry& entry, uint32_t physical, uint32_t guest_bytes, VkDeviceSize host_bytes,
               VkCommandBuffer upload_cb, Region& out, bool& failed) {
  failed = false;
  const uint64_t frame = g_vk.frame;
  entry.last_used = frame;
  const bool enabled = REXCVAR_GET(svr_native_geometry_cache) && g.arena;
  if (enabled && entry.generation == g.generation &&
      !write_watch::IsDirty(physical, guest_bytes, entry.watch_token)) {
    out = {g.arena, entry.offset};
    return nullptr;
  }
  if (enabled && entry.uploaded_frame != UINT64_MAX) {
    entry.streak = entry.uploaded_frame + 1 >= frame ? entry.streak + 1 : 1;
    if (entry.streak >= kDynamicStreak) {
      entry.dynamic_until = frame + kDynamicFrames;
      entry.streak = 0;
      entry.uploaded_frame = UINT64_MAX;
    }
  }
  entry.generation = UINT32_MAX;
  if (!enabled || frame < entry.dynamic_until) {
    const Upload upload = AllocateUpload(host_bytes, 16);
    failed = !upload.data;
    out = {upload.buffer, upload.offset};
    return upload.data;
  }
  const uint32_t watch_token = write_watch::Watch(physical, guest_bytes);
  const Upload staging = AllocateUpload(host_bytes, 16);
  VkDeviceSize offset;
  if (!staging.data || !AllocateArena(host_bytes, offset)) {
    failed = !staging.data;
    out = {staging.buffer, staging.offset};
    return staging.data;
  }
  const VkBufferCopy copy = {staging.offset, offset, host_bytes};
  g_vk.dfn->vkCmdCopyBuffer(upload_cb, staging.buffer, g.arena, 1, &copy);
  g.copied = true;
  entry.offset = offset;
  entry.watch_token = watch_token;
  entry.generation = g.generation;
  entry.uploaded_frame = frame;
  out = {g.arena, offset};
  return staging.data;
}

}  // namespace

bool Initialize() {
  if (!CreateArena()) {
    REXLOG_WARN("native renderer: no geometry arena; uploading every draw");
    g.arena = VK_NULL_HANDLE;
  }
  return true;
}

bool Vertices(uint32_t physical_address, uint32_t bytes, VkCommandBuffer upload_cb,
              Region& out) {
  if (g_vk.frame >= g.pruned_frame + kPruneAfterFrames) {
    g.pruned_frame = g_vk.frame;
    Prune(g.vertices, g_vk.frame);
    Prune(g.indices, g_vk.frame);
  }
  Entry& entry = g.vertices[uint64_t(physical_address) << 32 | bytes];
  const uint32_t words = (bytes + 3) / 4;
  bool failed;
  uint8_t* data = Place(entry, physical_address, bytes, VkDeviceSize(words) * 4, upload_cb, out,
                        failed);
  if (data) {
    guest::SwapCopy32(reinterpret_cast<uint32_t*>(data), TranslatePhysical(physical_address),
                      words);
  }
  return !failed;
}

bool IndexBuffer(uint32_t physical_address, uint32_t count, bool index32, bool quads,
                 VkCommandBuffer upload_cb, Indices& out) {
  const uint64_t key = uint64_t(physical_address) << 32 | uint64_t(count) << 2 |
                       (index32 ? 2 : 0) | (quads ? 1 : 0);
  Entry& entry = g.indices[key];
  const uint32_t draw_count = quads ? count / 4 * 6 : count;
  if (!draw_count) {
    return false;
  }
  bool failed;
  uint8_t* data = Place(entry, physical_address, count * (index32 ? 4 : 2),
                        VkDeviceSize(draw_count) * 4, upload_cb, out.region, failed);
  if (failed) {
    return false;
  }
  if (data) {
    g.index_scratch.resize(count);
    const guest::IndexRange range = guest::ScanIndices(
        TranslatePhysical(physical_address), count, index32, g.index_scratch.data());
    const uint32_t min_index = range.empty() ? 0 : range.min;
    entry.count = range.empty()
                      ? 0
                      : guest::RebaseIndices(g.index_scratch.data(), count, index32, quads,
                                             min_index, reinterpret_cast<uint32_t*>(data));
    entry.min_index = min_index;
    entry.max_index = range.empty() ? 0 : range.max;
  }
  out.count = entry.count;
  out.min_index = entry.min_index;
  out.max_index = entry.max_index;
  return out.count != 0;
}

void EndFrame(VkCommandBuffer upload_cb) {
  if (!g.copied) {
    return;
  }
  g.copied = false;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
  g_vk.dfn->vkCmdPipelineBarrier(upload_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr,
                                 0, nullptr);
}

}  // namespace svr::native::geometry
