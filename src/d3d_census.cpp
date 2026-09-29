// Counts calls to the game's XDK Direct3D functions that build GPU draw
// packets, so a session log can be compared with the PM4 draws the command
// processor executes (--gpu_draw_census). Equal totals mean every draw the GPU
// sees came through a function a native renderer can hook.
//
// Addresses are in the statically linked D3D library (0x82914800-0x82933240);
// see scripts/scan_pm4.py for how they were found.

#include "generated/default/svr2011_pch.h"

#include "native/native_renderer.h"
#include "native/shader_library.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(svr_d3d_census, false, "SVR2011",
                    "Log cumulative D3D draw-function call counts every 60 presents");
REXCVAR_DEFINE_STRING(svr_shader_dump_dir, "", "SVR2011",
                      "Save each unique shader container passed to CreateVertexShader or "
                      "CreatePixelShader to this directory, for offline conversion");

namespace {

enum Counter : int {
  kDrawIndexedVertices,  // 82921B58, PM4 DRAW_INDX
  kDrawVerticesUP,       // 82921698, PM4 DRAW_INDX (quad lists, 2D and UI)
  kClearRect,            // 8291EC48, PM4 DRAW_INDX_2 after each Clear
  kDraw14F68,            // unnamed DRAW_INDX_2 builders, not seen per frame so far
  kDraw150C0,
  kDraw1A468,
  kDraw21230,
  kDraw29158,
  kDraw2F338,
  kIndirectBuffer,  // 82922C58, PM4 INDIRECT_BUFFER
  kResolve,         // 82918A88
  kSetRenderTarget,  // 8291E618
  kCreateVertexShader,  // 82921548
  kCreatePixelShader,   // 82921360
  kCount
};

std::atomic<uint64_t> g_counts[kCount];
std::atomic<uint64_t> g_presents;

void Count(Counter c) { g_counts[c].fetch_add(1, std::memory_order_relaxed); }

uint64_t Get(Counter c) { return g_counts[c].load(std::memory_order_relaxed); }

void LogCensus(uint64_t presents) {
  const uint64_t other = Get(kDraw14F68) + Get(kDraw150C0) + Get(kDraw1A468) +
                         Get(kDraw21230) + Get(kDraw29158) + Get(kDraw2F338);
  REXLOG_INFO(
      "draw-census guest presents={} indexed={} up={} clear_rect={} other_draws={} "
      "(14F68={} 150C0={} 1A468={} 21230={} 29158={} 2F338={}) indirect={} resolve={} "
      "set_rt={} create_vs={} create_ps={}",
      presents, Get(kDrawIndexedVertices), Get(kDrawVerticesUP), Get(kClearRect), other,
      Get(kDraw14F68), Get(kDraw150C0), Get(kDraw1A468), Get(kDraw21230), Get(kDraw29158),
      Get(kDraw2F338), Get(kIndirectBuffer), Get(kResolve), Get(kSetRenderTarget),
      Get(kCreateVertexShader), Get(kCreatePixelShader));
}

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Shader containers start with flags 0x102A11xx, then the virtual (header and
// constant tables) and physical (microcode) sizes; the two parts are contiguous.
void DumpShaderContainer(const uint8_t* base, uint32_t guest_address, const char* kind) {
  const std::string dir = REXCVAR_GET(svr_shader_dump_dir);
  if (dir.empty() || !guest_address) {
    return;
  }
  const uint8_t* container = base + guest_address;
  const uint32_t flags = LoadBE32(container);
  const uint32_t size = LoadBE32(container + 4) + LoadBE32(container + 8);
  if ((flags & 0xFFFFFF00) != 0x102A1100 || size == 0 || size > (1u << 20)) {
    REXLOG_WARN("shader dump: {} container at {:08X} has flags {:08X} size {}", kind,
                guest_address, flags, size);
    return;
  }
  uint64_t hash = 14695981039346656037ull;  // FNV-1a
  for (uint32_t i = 0; i < size; ++i) {
    hash = (hash ^ container[i]) * 1099511628211ull;
  }
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard lock(mutex);
  if (!seen.insert(hash).second) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  const auto path =
      std::filesystem::path(dir) / fmt::format("{}_{:016x}.bin", kind, hash);
  if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(container, 1, size, file);
    std::fclose(file);
  }
}

}  // namespace

#define SVR_COUNTING_HOOK(address, counter) \
  DECLARE_REX_FUNC(sub_##address);          \
  REX_HOOK_RAW(sub_##address) {             \
    Count(counter);                         \
    __imp__sub_##address(ctx, base);        \
  }

// D3DDevice_DrawIndexedVertices / DrawVerticesUP: r3 = device, whose register
// mirror holds the draw's complete state.
DECLARE_REX_FUNC(sub_82921B58);
REX_HOOK_RAW(sub_82921B58) {
  Count(kDrawIndexedVertices);
  svr::native::OnDraw(base, ctx.r3.u32);
  __imp__sub_82921B58(ctx, base);
}

DECLARE_REX_FUNC(sub_82921698);
REX_HOOK_RAW(sub_82921698) {
  Count(kDrawVerticesUP);
  svr::native::OnDraw(base, ctx.r3.u32);
  __imp__sub_82921698(ctx, base);
}

SVR_COUNTING_HOOK(8291EC48, kClearRect)
SVR_COUNTING_HOOK(82914F68, kDraw14F68)
SVR_COUNTING_HOOK(829150C0, kDraw150C0)
SVR_COUNTING_HOOK(8291A468, kDraw1A468)
SVR_COUNTING_HOOK(82921230, kDraw21230)
SVR_COUNTING_HOOK(82929158, kDraw29158)
SVR_COUNTING_HOOK(8292F338, kDraw2F338)
SVR_COUNTING_HOOK(82922C58, kIndirectBuffer)
SVR_COUNTING_HOOK(82918A88, kResolve)
SVR_COUNTING_HOOK(8291E618, kSetRenderTarget)

// D3DDevice_CreateVertexShader / CreatePixelShader: r3 = shader container.
DECLARE_REX_FUNC(sub_82921548);
REX_HOOK_RAW(sub_82921548) {
  Count(kCreateVertexShader);
  const uint32_t container = ctx.r3.u32;
  DumpShaderContainer(base, container, "vs");
  __imp__sub_82921548(ctx, base);
  // Returns the shader object in r3.
  svr::native::shader_library::OnShaderCreated(base + container, ctx.r3.u32, false);
}

DECLARE_REX_FUNC(sub_82921360);
REX_HOOK_RAW(sub_82921360) {
  Count(kCreatePixelShader);
  const uint32_t container = ctx.r3.u32;
  DumpShaderContainer(base, container, "ps");
  __imp__sub_82921360(ctx, base);
  svr::native::shader_library::OnShaderCreated(base + container, ctx.r3.u32, true);
}

// D3DDevice_Present: the only caller of VdSwap, once per frame.
DECLARE_REX_FUNC(sub_8291AED0);
REX_HOOK_RAW(sub_8291AED0) {
  svr::native::OnPresent();
  __imp__sub_8291AED0(ctx, base);
  const uint64_t presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (presents % 60 == 0 && REXCVAR_GET(svr_d3d_census)) {
    LogCensus(presents);
  }
}
