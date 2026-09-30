// Which guest physical pages changed since the native renderer last read
// them: pages it caches (textures, vertices, indices) are write-protected
// through the SDK's physical memory access callbacks, as the emulator's
// shared memory does, and each write stamps its pages with a new epoch. A
// cache entry keeps the epoch from when it watched its range, so entries
// sharing pages never hide each other's writes.
//
// Pages the guest has made read-only are not protected by the SDK and so
// never report writes (as in the emulator); the svr_native_*_cache switches
// turn the caches off if a game relies on reprotecting its data.

#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr::native::write_watch {

// Registers the invalidation callback. Without it every range reads dirty.
bool Initialize(rex::memory::Memory* memory);

// Write-protects [address, address + length) and returns the token IsDirty
// compares against. Read the data after this call: a write that lands before
// the read is in the data, one after it makes the range dirty.
uint32_t Watch(uint32_t physical_address, uint32_t length);

// True if a page of the range was written after the Watch that returned token.
bool IsDirty(uint32_t physical_address, uint32_t length, uint32_t token);

// Host writes into guest memory (resolve readbacks) go around the page
// protection; this marks their pages written.
void MarkWritten(uint32_t physical_address, uint32_t length);

// The pure page stamps behind these, for tests: the epoch of each 4 KB
// page's last write (0 = none seen).
class PageStamps {
 public:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPages = 0x20000000u >> kPageShift;  // 512 MB

  // Written by guest threads (Stamp) and the render thread.
  void Stamp(uint32_t address, uint32_t length, uint32_t epoch);
  bool WrittenAfter(uint32_t address, uint32_t length, uint32_t token) const;

 private:
  uint32_t stamps_[kPages] = {};
};

}  // namespace svr::native::write_watch
