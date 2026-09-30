#include "native/texture_layout.h"

#include <cstring>
#include <utility>

#include <rex/graphics/pipeline/texture/util.h>

namespace svr::native::texture_layout {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

void EndianSwap(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
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

void CopyLevelFromGuest(uint8_t* dest, const uint8_t* source, uint32_t bx, uint32_t by,
                        uint32_t bytes_per_block_log2, uint32_t row_pitch_bytes, bool tiled,
                        uint32_t tail_x, uint32_t tail_y) {
  const uint32_t bytes_per_block = 1u << bytes_per_block_log2;
  const size_t row_bytes = size_t(bx) * bytes_per_block;
  const uint32_t pitch_blocks = row_pitch_bytes >> bytes_per_block_log2;
  for (uint32_t y = 0; y < by; ++y) {
    uint8_t* row = dest + size_t(y) * row_bytes;
    if (tiled) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t offset = texture_util::GetTiledOffset2D(
            int32_t(x + tail_x), int32_t(y + tail_y), pitch_blocks, bytes_per_block_log2);
        std::memcpy(row + size_t(x) * bytes_per_block, source + offset, bytes_per_block);
      }
    } else {
      std::memcpy(row,
                  source + size_t(y + tail_y) * row_pitch_bytes + size_t(tail_x) * bytes_per_block,
                  row_bytes);
    }
  }
}

void Downsample(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                uint32_t scale, uint32_t bytes_per_pixel, bool average_bytes, uint8_t* dest) {
  const uint32_t width = source_width / scale;
  const uint32_t height = source_height / scale;
  const size_t source_row = size_t(source_width) * bytes_per_pixel;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* block =
          source + size_t(y) * scale * source_row + size_t(x) * scale * bytes_per_pixel;
      uint8_t* out = dest + (size_t(y) * width + x) * bytes_per_pixel;
      if (!average_bytes) {
        std::memcpy(out, block, bytes_per_pixel);
        continue;
      }
      for (uint32_t b = 0; b < bytes_per_pixel; ++b) {
        uint32_t sum = 0;
        for (uint32_t sy = 0; sy < scale; ++sy) {
          for (uint32_t sx = 0; sx < scale; ++sx) {
            sum += block[sy * source_row + sx * bytes_per_pixel + b];
          }
        }
        const uint32_t count = scale * scale;
        out[b] = uint8_t((sum + count / 2) / count);
      }
    }
  }
}

uint32_t LinearRowBytes(uint32_t pitch_texels, uint32_t bytes_per_block_log2) {
  return ((pitch_texels << bytes_per_block_log2) + xenos::kTextureLinearRowAlignmentBytes - 1) &
         ~(xenos::kTextureLinearRowAlignmentBytes - 1);
}

uint32_t GuestOffset(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_block_log2,
                     bool tiled, uint32_t linear_row_bytes) {
  if (tiled) {
    return uint32_t(texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_texels,
                                                   bytes_per_block_log2));
  }
  return y * linear_row_bytes + (x << bytes_per_block_log2);
}

}  // namespace svr::native::texture_layout
