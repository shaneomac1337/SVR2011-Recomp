// Which guest physical pages changed since the native renderer last read
// them: pages it caches (textures, vertices, indices) are write-protected
// through the SDK's physical memory access callbacks, as the emulator's
// shared memory does, and a write clears their "clean" bit.

#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr::native::write_watch {

// Registers the invalidation callback. Without it every range reads dirty.
bool Initialize(rex::memory::Memory* memory);

// Marks [address, address + length) clean and write-protects it. Read the
// data after this call: a write that lands before the read is in the data,
// one after it makes the range dirty again.
void Watch(uint32_t physical_address, uint32_t length);

// True if any page of the range was written, or never watched, since Watch.
bool IsDirty(uint32_t physical_address, uint32_t length);

// The pure page bitmap behind both, for tests: pages are 4 KB, bits set = clean.
class PageBitmap {
 public:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPages = 0x20000000u >> kPageShift;  // 512 MB

  void MarkClean(uint32_t address, uint32_t length);
  void MarkDirty(uint32_t address, uint32_t length);
  bool AllClean(uint32_t address, uint32_t length) const;

 private:
  template <typename F>
  static void ForBlocks(uint32_t address, uint32_t length, F&& f);
  // Written by guest threads (MarkDirty) and the render thread.
  alignas(64) uint64_t bits_[kPages / 64] = {};
};

}  // namespace svr::native::write_watch
