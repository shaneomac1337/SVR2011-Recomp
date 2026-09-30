// Guest texture memory layout for the native renderer: un-tiling levels into
// tightly packed rows, tiling resolved pixels back, and the endian swaps.
// Pure CPU code over the SDK's Xenos tiling (texture_util), tested offline.

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/graphics/xenos.h>

namespace svr::native::texture_layout {

// Byte order conversion between guest and host order; the same swap works in
// both directions.
void EndianSwap(uint8_t* data, size_t size, rex::graphics::xenos::Endian endian);

// Copies bx x by blocks of one level (or packed-tail level at tail_x, tail_y)
// from guest memory into tightly packed rows. row_pitch_bytes is the stored
// level's pitch; tiled levels use the Xenos 2D tiling.
void CopyLevelFromGuest(uint8_t* dest, const uint8_t* source, uint32_t bx, uint32_t by,
                        uint32_t bytes_per_block_log2, uint32_t row_pitch_bytes, bool tiled,
                        uint32_t tail_x = 0, uint32_t tail_y = 0);

// Byte offset of texel (x, y) in a guest level: tiled, or linear rows of
// linear_row_bytes.
uint32_t GuestOffset(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_block_log2,
                     bool tiled, uint32_t linear_row_bytes);

// Shrinks tightly packed pixels by an integer scale: each scale x scale block
// becomes one pixel, its bytes averaged when every byte is an 8-bit unorm
// channel (average_bytes), else the block's top-left pixel.
void Downsample(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                uint32_t scale, uint32_t bytes_per_pixel, bool average_bytes, uint8_t* dest);

// Linear guest rows are padded to kTextureLinearRowAlignmentBytes.
uint32_t LinearRowBytes(uint32_t pitch_texels, uint32_t bytes_per_block_log2);

}  // namespace svr::native::texture_layout
