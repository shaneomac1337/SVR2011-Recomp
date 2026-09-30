#include <catch_amalgamated.hpp>

#include <cstring>
#include <set>
#include <vector>

#include "native/texture_layout.h"

using namespace svr::native::texture_layout;
using rex::graphics::xenos::Endian;

TEST_CASE("endian swaps") {
  uint8_t data[4] = {1, 2, 3, 4};
  EndianSwap(data, 4, Endian::k8in16);
  CHECK(std::memcmp(data, "\x02\x01\x04\x03", 4) == 0);
  const uint8_t original[4] = {1, 2, 3, 4};
  std::memcpy(data, original, 4);
  EndianSwap(data, 4, Endian::k8in32);
  CHECK(std::memcmp(data, "\x04\x03\x02\x01", 4) == 0);
  std::memcpy(data, original, 4);
  EndianSwap(data, 4, Endian::k16in32);
  CHECK(std::memcmp(data, "\x03\x04\x01\x02", 4) == 0);
  std::memcpy(data, original, 4);
  EndianSwap(data, 4, Endian::kNone);
  CHECK(std::memcmp(data, original, 4) == 0);
}

TEST_CASE("linear rows are 256-byte aligned") {
  CHECK(LinearRowBytes(1280, 2) == 5120);
  CHECK(LinearRowBytes(100, 2) == 512);
  CHECK(LinearRowBytes(32, 0) == 256);
}

TEST_CASE("tiled offsets of a level never collide") {
  for (const uint32_t bpb_log2 : {0u, 1u, 2u, 3u, 4u}) {
    const uint32_t pitch = 64, height = 64;
    std::set<uint32_t> offsets;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < pitch; ++x) {
        const uint32_t offset = GuestOffset(x, y, pitch, bpb_log2, true, 0);
        CHECK((offset & ((1u << bpb_log2) - 1)) == 0);
        offsets.insert(offset);
      }
    }
    CHECK(offsets.size() == pitch * height);
    CHECK(*offsets.rbegin() < (pitch * height) << bpb_log2);
  }
}

// Writes each block's index into a guest level laid out as GuestOffset says,
// then reads it back as the renderer's upload does.
static void RoundTrip(bool tiled, uint32_t tail_x, uint32_t tail_y) {
  const uint32_t bpb_log2 = 2, pitch = 128, rows = 64, bx = 24, by = 16;
  const uint32_t linear_row_bytes = LinearRowBytes(pitch, bpb_log2);
  std::vector<uint8_t> guest(size_t(linear_row_bytes) * rows * 2, 0xCD);
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const uint32_t value = y * bx + x;
      std::memcpy(guest.data() + GuestOffset(x + tail_x, y + tail_y, pitch, bpb_log2, tiled,
                                             linear_row_bytes),
                  &value, 4);
    }
  }
  std::vector<uint32_t> host(bx * by);
  CopyLevelFromGuest(reinterpret_cast<uint8_t*>(host.data()), guest.data(), bx, by, bpb_log2,
                     tiled ? pitch << bpb_log2 : linear_row_bytes, tiled, tail_x, tail_y);
  for (uint32_t i = 0; i < bx * by; ++i) {
    REQUIRE(host[i] == i);
  }
}

TEST_CASE("levels read back from guest layouts") {
  RoundTrip(false, 0, 0);
  RoundTrip(true, 0, 0);
  // Packed mip tail levels sit at an offset inside the stored level.
  RoundTrip(false, 16, 8);
  RoundTrip(true, 32, 16);
}
