#include "native/write_watch.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include <rex/system/xmemory.h>

namespace svr::native::write_watch {

template <typename F>
void PageBitmap::ForBlocks(uint32_t address, uint32_t length, F&& f) {
  if (!length || address >= 0x20000000u) {
    return;
  }
  const uint64_t end = std::min<uint64_t>(uint64_t(address) + length, 0x20000000u);
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t((end - 1) >> kPageShift);
  for (uint32_t block = first >> 6; block <= last >> 6; ++block) {
    uint64_t mask = ~0ull;
    if (block == first >> 6) {
      mask &= ~0ull << (first & 63);
    }
    if (block == last >> 6) {
      mask &= ~0ull >> (63 - (last & 63));
    }
    f(block, mask);
  }
}

void PageBitmap::MarkClean(uint32_t address, uint32_t length) {
  ForBlocks(address, length, [this](uint32_t block, uint64_t mask) {
    std::atomic_ref<uint64_t>(bits_[block]).fetch_or(mask, std::memory_order_relaxed);
  });
}

void PageBitmap::MarkDirty(uint32_t address, uint32_t length) {
  ForBlocks(address, length, [this](uint32_t block, uint64_t mask) {
    std::atomic_ref<uint64_t>(bits_[block]).fetch_and(~mask, std::memory_order_relaxed);
  });
}

bool PageBitmap::AllClean(uint32_t address, uint32_t length) const {
  if (!length || address >= 0x20000000u) {
    return false;
  }
  bool clean = true;
  ForBlocks(address, length, [this, &clean](uint32_t block, uint64_t mask) {
    const uint64_t bits = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(bits_[block]))
                              .load(std::memory_order_acquire);
    clean = clean && (bits & mask) == mask;
  });
  return clean;
}

namespace {

rex::memory::Memory* g_memory = nullptr;
std::unique_ptr<PageBitmap> g_pages;

// Guest threads, under the global critical region: the written range becomes
// dirty. Returning exactly that range keeps the other watchers' pages around
// it protected.
std::pair<uint32_t, uint32_t> OnInvalidate(void*, uint32_t physical_address_start,
                                           uint32_t length, bool) {
  g_pages->MarkDirty(physical_address_start, length);
  return {physical_address_start, length};
}

}  // namespace

bool Initialize(rex::memory::Memory* memory) {
  if (g_memory) {
    return true;
  }
  g_pages = std::make_unique<PageBitmap>();
  g_memory = memory;
  memory->RegisterPhysicalMemoryInvalidationCallback(OnInvalidate, nullptr);
  return true;
}

void Watch(uint32_t physical_address, uint32_t length) {
  if (!g_memory || !length) {
    return;
  }
  g_pages->MarkClean(physical_address, length);
  g_memory->EnablePhysicalMemoryAccessCallbacks(physical_address, length, true, false);
}

bool IsDirty(uint32_t physical_address, uint32_t length) {
  return !g_memory || !g_pages->AllClean(physical_address, length);
}

}  // namespace svr::native::write_watch
