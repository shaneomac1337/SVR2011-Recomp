#include "native/write_watch.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include <rex/system/xmemory.h>

namespace svr::native::write_watch {

namespace {

// Pages of [address, address + length) within physical memory; false if none.
bool PageRange(uint32_t address, uint32_t length, uint32_t& first, uint32_t& last) {
  if (!length || address >= 0x20000000u) {
    return false;
  }
  const uint64_t end = std::min<uint64_t>(uint64_t(address) + length, 0x20000000u);
  first = address >> PageStamps::kPageShift;
  last = uint32_t((end - 1) >> PageStamps::kPageShift);
  return true;
}

}  // namespace

void PageStamps::Stamp(uint32_t address, uint32_t length, uint32_t epoch) {
  uint32_t first, last;
  if (!PageRange(address, length, first, last)) {
    return;
  }
  for (uint32_t page = first; page <= last; ++page) {
    std::atomic_ref<uint32_t> stamp(stamps_[page]);
    uint32_t current = stamp.load(std::memory_order_relaxed);
    while (current < epoch &&
           !stamp.compare_exchange_weak(current, epoch, std::memory_order_release)) {
    }
  }
}

bool PageStamps::WrittenAfter(uint32_t address, uint32_t length, uint32_t token) const {
  uint32_t first, last;
  if (!PageRange(address, length, first, last)) {
    return true;
  }
  for (uint32_t page = first; page <= last; ++page) {
    if (std::atomic_ref<uint32_t>(const_cast<uint32_t&>(stamps_[page]))
            .load(std::memory_order_acquire) > token) {
      return true;
    }
  }
  return false;
}

namespace {

rex::memory::Memory* g_memory = nullptr;
std::unique_ptr<PageStamps> g_pages;
std::atomic<uint32_t> g_epoch{0};

// Guest threads, under the global critical region: the written range gets a
// new epoch. Returning exactly that range keeps the other watchers' pages
// around it protected.
std::pair<uint32_t, uint32_t> OnInvalidate(void*, uint32_t physical_address_start,
                                           uint32_t length, bool) {
  g_pages->Stamp(physical_address_start, length, g_epoch.fetch_add(1) + 1);
  return {physical_address_start, length};
}

}  // namespace

bool Initialize(rex::memory::Memory* memory) {
  if (g_memory) {
    return true;
  }
  g_pages = std::make_unique<PageStamps>();
  g_memory = memory;
  memory->RegisterPhysicalMemoryInvalidationCallback(OnInvalidate, nullptr);
  return true;
}

uint32_t Watch(uint32_t physical_address, uint32_t length) {
  if (!g_memory || !length) {
    return 0;
  }
  // Writes stamped up to now are in the data read after this call.
  const uint32_t token = g_epoch.load(std::memory_order_acquire);
  g_memory->EnablePhysicalMemoryAccessCallbacks(physical_address, length, true, false);
  return token;
}

bool IsDirty(uint32_t physical_address, uint32_t length, uint32_t token) {
  return !g_memory || g_pages->WrittenAfter(physical_address, length, token);
}

void MarkWritten(uint32_t physical_address, uint32_t length) {
  if (g_memory) {
    g_pages->Stamp(physical_address, length, g_epoch.fetch_add(1) + 1);
  }
}

}  // namespace svr::native::write_watch
