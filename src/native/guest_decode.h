// Pure decoding of guest (Xbox 360) data for the native renderer: big-endian
// loads, CPU to physical addresses, the D3D device's register mirror, vertex
// declaration types, render-target formats and index buffers. No Vulkan calls
// and no state, so tests/native checks it offline.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/ui/vulkan/api.h>

namespace svr::native::guest {

inline uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline uint16_t LoadBE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

inline float LoadBEFloat(const uint8_t* p) {
  const uint32_t bits = LoadBE32(p);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

inline uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

inline void SwapCopy32(uint32_t* dst, const uint8_t* src, size_t dwords) {
  for (size_t i = 0; i < dwords; ++i) {
    dst[i] = LoadBE32(src + i * 4);
  }
}

// Objects (index buffers, texture objects) hold CPU addresses; the GPU sees
// physical ones, with the 0xE0000000 range offset by 0x1000 as D3D computes
// it for the draw packet. Device fetch constants already hold physical ones.
constexpr uint32_t ToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
}

// --- Register mirror --------------------------------------------------------

// Guest D3DDevice register mirror groups (SvR 2011's XDK, verified with
// --svr_d3d_probe): first register, device offset, count.
struct MirrorGroup {
  uint32_t first;
  uint32_t offset;
  uint32_t count;
};
constexpr MirrorGroup kMirrorGroups[] = {
    {0x2000, 0x2880, 19}, {0x2100, 0x28CC, 21}, {0x2180, 0x2920, 5},  {0x2200, 0x2934, 12},
    {0x2280, 0x2964, 21}, {0x2300, 0x29B8, 38}, {0x2380, 0x2A50, 8},
};

// Device offset of a mirrored register, or 0 when the device keeps no copy.
constexpr uint32_t MirrorOffset(uint32_t reg) {
  for (const MirrorGroup& group : kMirrorGroups) {
    if (reg >= group.first && reg < group.first + group.count) {
      return group.offset + (reg - group.first) * 4;
    }
  }
  return 0;
}

inline uint32_t ReadReg(const uint8_t* d3d, uint32_t reg) {
  const uint32_t offset = MirrorOffset(reg);
  return offset ? LoadBE32(d3d + offset) : 0;
}

inline float ReadRegFloat(const uint8_t* d3d, uint32_t reg) {
  const uint32_t bits = ReadReg(d3d, reg);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// --- Vertex declarations ----------------------------------------------------

// XenosRecomp's SPIR-V vertex input locations (non-re:Blue table), and whether
// the shader declares the input as uint4.
struct InputLocation {
  uint8_t usage;
  uint8_t index;
  uint8_t location;
  bool is_uint;
};
constexpr InputLocation kInputLocations[] = {
    {0, 0, 0, false},   // POSITION0
    {3, 0, 1, true},    // NORMAL0
    {6, 0, 2, true},    // TANGENT0
    {7, 0, 3, true},    // BINORMAL0
    {5, 0, 4, false},   // TEXCOORD0
    {5, 1, 5, false},   // TEXCOORD1
    {5, 2, 6, false},   // TEXCOORD2
    {5, 3, 7, false},   // TEXCOORD3
    {10, 0, 8, false},  // COLOR0
    {2, 0, 9, true},    // BLENDINDICES0
    {1, 0, 10, false},  // BLENDWEIGHT0
    {10, 1, 11, false}, // COLOR1
    {5, 4, 12, false},  // TEXCOORD4
    {5, 5, 13, false},  // TEXCOORD5
    {5, 6, 14, false},  // TEXCOORD6
    {5, 7, 15, false},  // TEXCOORD7
};

struct DeclType {
  uint32_t type;
  VkFormat float_format;
  VkFormat uint_format;
  bool sixteen_bit;
  bool packed_normal;  // 10_11_11 / 11_11_10, decoded by the shader
};
constexpr DeclType kDeclTypes[] = {
    {0x2C83A4, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, false, false},
    {0x2C23A5, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, false, false},
    {0x2A23B9, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, false, false},
    {0x1A23A6, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, false, false},
    {0x182886, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UINT, false, false},
    {0x1A2286, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_UINT, false, false},
    {0x1A2386, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_UINT, false, false},
    {0x1A2086, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, false, false},
    {0x1A2186, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, false, false},
    {0x2C2359, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, true, false},
    {0x1A235A, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_UINT, true, false},
    {0x2C2159, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, true, false},
    {0x1A215A, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, true, false},
    {0x2C2059, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UINT, true, false},
    {0x1A205A, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UINT, true, false},
    {0x2C82A1, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, false, false},
    {0x2A2287, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_R32_UINT, false, false},
    {0x2A2187, VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_R32_UINT, false, false},
    // Signed normalized 11_11_10 normals (X 10 bits, Y and Z 11): raw uint,
    // decoded by the shader. Its decoder knows no other packed layout.
    {0x2A2191, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, false, true},
    {0x2C235F, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UINT, true, false},
    {0x1A2360, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT, true, false},
};

constexpr const DeclType* FindDeclType(uint32_t type) {
  for (const DeclType& t : kDeclTypes) {
    if (t.type == type) {
      return &t;
    }
  }
  return nullptr;
}

constexpr const InputLocation* FindLocation(uint32_t usage, uint32_t index) {
  for (const InputLocation& l : kInputLocations) {
    if (l.usage == usage && l.index == index) {
      return &l;
    }
  }
  return nullptr;
}

// --- Render targets ---------------------------------------------------------

// Host format of a Xenos ColorRenderTargetFormat (RB_COLOR_INFO bits 16..19).
constexpr VkFormat ColorTargetFormat(uint32_t color_format) {
  switch (color_format) {
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:   // k_2_10_10_10_FLOAT
    case 7:   // k_16_16_16_16_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4:
      return VK_FORMAT_R16G16_SNORM;
    case 5:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:
      return VK_FORMAT_R16G16_SFLOAT;
    case 14:
      return VK_FORMAT_R32_SFLOAT;
    case 15:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // k_8_8_8_8, k_8_8_8_8_GAMMA
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

// Surface objects: packed size (width - 1 in bits 31:18, height - 1 in 17:3).
struct SurfaceSize {
  uint32_t width;
  uint32_t height;
};
constexpr SurfaceSize DecodeSurfaceSize(uint32_t packed) {
  return {(packed >> 18) + 1, ((packed >> 3) & 0x7FFF) + 1};
}

// --- Clip space -------------------------------------------------------------

// How a draw's positions reach the target. The converted vertex shaders end
// with oPos.xy = oPos.xy * ndc_scale + offset * oPos.w, then the viewport
// (x_scale, x_offset, y_scale, y_offset: D3D's form, in guest pixels) maps
// clip space to pixels. With PA_CL_VTE_CNTL's scales on, positions are in clip
// space and the guest viewport is used as is. With them off, positions are
// already in pixels (the game's full-screen passes); Vulkan clips before the
// viewport, so the shader maps them to clip space for a full-target viewport.
struct ClipTransform {
  float ndc_scale[2];
  float offset[2];
  float x_scale, x_offset, y_scale, y_offset;
};

// half_pixel: shift by half a guest pixel right and down (D3D9 pixel centres).
inline ClipTransform ComputeClipTransform(uint32_t vte, float x_scale, float x_offset,
                                          float y_scale, float y_offset, float target_width,
                                          float target_height, bool half_pixel) {
  const float sx = (vte & 1) ? x_scale : 1.0f;
  const float ox = (vte & 2) ? x_offset : 0.0f;
  const float sy = (vte & 4) ? y_scale : 1.0f;
  const float oy = (vte & 8) ? y_offset : 0.0f;
  ClipTransform t;
  if ((vte & 1) && (vte & 4)) {
    t.ndc_scale[0] = t.ndc_scale[1] = 1.0f;
    t.offset[0] = half_pixel && sx != 0.0f ? 0.5f / sx : 0.0f;
    t.offset[1] = half_pixel && sy != 0.0f ? 0.5f / sy : 0.0f;
    t.x_scale = sx;
    t.x_offset = ox;
    t.y_scale = sy;
    t.y_offset = oy;
    return t;
  }
  // pixel = position / w * s + o on each axis, into a viewport of the whole
  // target (clip x -1..1 left to right, y 1..-1 top to bottom).
  const float half_w = target_width * 0.5f, half_h = target_height * 0.5f;
  t.ndc_scale[0] = sx / half_w;
  t.offset[0] = (ox - half_w) / half_w + (half_pixel ? 1.0f / target_width : 0.0f);
  t.ndc_scale[1] = -sy / half_h;
  t.offset[1] = (half_h - oy) / half_h - (half_pixel ? 1.0f / target_height : 0.0f);
  t.x_scale = half_w;
  t.x_offset = half_w;
  t.y_scale = -half_h;
  t.y_offset = half_h;
  return t;
}

// --- Index buffers ----------------------------------------------------------

// The range of vertices a run of indices touches, skipping the restart index.
struct IndexRange {
  uint32_t min = UINT32_MAX;
  uint32_t max = 0;
  bool empty() const { return min > max; }
};
inline IndexRange ScanIndices(const uint8_t* big_endian, uint32_t count, bool index32,
                              uint32_t* out) {
  const uint32_t reset = index32 ? 0xFFFFFFFFu : 0xFFFFu;
  IndexRange range;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t index = index32 ? LoadBE32(big_endian + i * 4) : LoadBE16(big_endian + i * 2);
    out[i] = index;
    if (index != reset) {
      range.min = index < range.min ? index : range.min;
      range.max = index > range.max ? index : range.max;
    }
  }
  return range;
}

// Indices rebased to the first vertex of range, restart indices to all ones,
// quad lists split into two triangles each. Returns the count written.
inline uint32_t RebaseIndices(const uint32_t* indices, uint32_t count, bool index32, bool quads,
                              uint32_t min_index, uint32_t* out) {
  const uint32_t reset = index32 ? 0xFFFFFFFFu : 0xFFFFu;
  auto rebase = [&](uint32_t index) { return index == reset ? 0xFFFFFFFFu : index - min_index; };
  if (!quads) {
    for (uint32_t i = 0; i < count; ++i) {
      out[i] = rebase(indices[i]);
    }
    return count;
  }
  uint32_t o = 0;
  for (uint32_t i = 0; i + 3 < count; i += 4, o += 6) {
    const uint32_t* q = indices + i;
    out[o + 0] = rebase(q[0]);
    out[o + 1] = rebase(q[1]);
    out[o + 2] = rebase(q[2]);
    out[o + 3] = rebase(q[0]);
    out[o + 4] = rebase(q[2]);
    out[o + 5] = rebase(q[3]);
  }
  return o;
}

}  // namespace svr::native::guest
