#include <catch_amalgamated.hpp>

#include "native/guest_decode.h"

using namespace svr::native::guest;

TEST_CASE("big-endian loads") {
  const uint8_t bytes[] = {0x3F, 0x80, 0x00, 0x00, 0x12, 0x34};
  CHECK(LoadBE32(bytes) == 0x3F800000u);
  CHECK(LoadBE16(bytes + 4) == 0x1234u);
  CHECK(LoadBEFloat(bytes) == 1.0f);
  CHECK(ByteSwap32(0x11223344u) == 0x44332211u);
  uint32_t out[1];
  SwapCopy32(out, bytes, 1);
  CHECK(out[0] == 0x3F800000u);
}

TEST_CASE("CPU addresses translate to physical like the draw packet") {
  CHECK(ToPhysical(0xA0001000u) == 0x00001000u);
  CHECK(ToPhysical(0x40000000u) == 0x00000000u);
  CHECK(ToPhysical(0xBFFFF000u) == 0x1FFFF000u);
  // The 0xE0000000 range is offset by one page.
  CHECK(ToPhysical(0xE0000000u) == 0x00001000u);
  CHECK(ToPhysical(0xE0010000u) == 0x00011000u);
}

TEST_CASE("register mirror offsets match the verified device layout") {
  CHECK(MirrorOffset(0x2000) == 0x2880);  // RB_SURFACE_INFO
  CHECK(MirrorOffset(0x2001) == 0x2884);  // RB_COLOR_INFO
  CHECK(MirrorOffset(0x210F) == 0x2908);  // PA_CL_VPORT_XSCALE
  CHECK(MirrorOffset(0x2200) == 0x2934);  // RB_DEPTHCONTROL
  CHECK(MirrorOffset(0x2318) == 0x2A18);  // RB_COPY_CONTROL
  CHECK(MirrorOffset(0x2380) == 0x2A50);
  // Past the end of a group, and registers the device keeps no copy of.
  CHECK(MirrorOffset(0x2013) == 0);
  CHECK(MirrorOffset(0x4900) == 0);

  uint8_t device[0x3000] = {};
  device[0x2884] = 0x12;
  device[0x2887] = 0x34;
  CHECK(ReadReg(device, 0x2001) == 0x12000034u);
  CHECK(ReadReg(device, 0x4900) == 0u);
}

TEST_CASE("declaration types and input locations") {
  const DeclType* normal = FindDeclType(0x2A2191);
  REQUIRE(normal);
  CHECK(normal->packed_normal);
  CHECK(normal->uint_format == VK_FORMAT_R32_UINT);
  const DeclType* half2 = FindDeclType(0x2C235F);
  REQUIRE(half2);
  CHECK(half2->sixteen_bit);
  CHECK(FindDeclType(0x123456) == nullptr);

  const InputLocation* texcoord1 = FindLocation(5, 1);
  REQUIRE(texcoord1);
  CHECK(texcoord1->location == 5);
  CHECK(FindLocation(3, 0)->is_uint);  // NORMAL0
  CHECK(FindLocation(12, 0) == nullptr);
}

TEST_CASE("render target formats") {
  CHECK(ColorTargetFormat(0) == VK_FORMAT_R8G8B8A8_UNORM);
  CHECK(ColorTargetFormat(14) == VK_FORMAT_R32_SFLOAT);
  CHECK(ColorTargetFormat(7) == VK_FORMAT_R16G16B16A16_SFLOAT);
}

TEST_CASE("surface sizes") {
  const SurfaceSize size = DecodeSurfaceSize((1279u << 18) | (719u << 3));
  CHECK(size.width == 1280);
  CHECK(size.height == 720);
}

TEST_CASE("index scan skips restart indices and rebases") {
  // 16-bit strip with a restart: 7 8 9 FFFF 5 6
  const uint8_t data[] = {0, 7, 0, 8, 0, 9, 0xFF, 0xFF, 0, 5, 0, 6};
  uint32_t indices[6];
  const IndexRange range = ScanIndices(data, 6, false, indices);
  CHECK(range.min == 5);
  CHECK(range.max == 9);
  uint32_t out[6];
  CHECK(RebaseIndices(indices, 6, false, false, range.min, out) == 6);
  CHECK(out[0] == 2);
  CHECK(out[3] == 0xFFFFFFFFu);
  CHECK(out[4] == 0);

  uint32_t none[1];
  const uint8_t only_restart[] = {0xFF, 0xFF};
  CHECK(ScanIndices(only_restart, 1, false, none).empty());
}

TEST_CASE("quad lists become two triangles each") {
  const uint32_t quad[8] = {10, 11, 12, 13, 14, 15, 16, 17};
  uint32_t out[12];
  CHECK(RebaseIndices(quad, 8, true, true, 10, out) == 12);
  const uint32_t expected[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
  for (int i = 0; i < 12; ++i) {
    CHECK(out[i] == expected[i]);
  }
}

// Where a clip-space position lands in pixels through a ClipTransform.
static void Project(const ClipTransform& t, float x, float y, float w, float& px, float& py) {
  const float cx = x * t.ndc_scale[0] + t.offset[0] * w;
  const float cy = y * t.ndc_scale[1] + t.offset[1] * w;
  px = cx / w * t.x_scale + t.x_offset;
  py = cy / w * t.y_scale + t.y_offset;
}

TEST_CASE("clip transform keeps the guest viewport for clip-space draws") {
  const ClipTransform t = ComputeClipTransform(0x43F, 640, 640, -360, 360, 1280, 720, false);
  float px, py;
  Project(t, -1, 1, 1, px, py);
  CHECK(px == Catch::Approx(0));
  CHECK(py == Catch::Approx(0));
  Project(t, 0.5f, -0.5f, 2, px, py);  // w = 2: (0.25, -0.25) in NDC
  CHECK(px == Catch::Approx(800));
  CHECK(py == Catch::Approx(450));
  const ClipTransform h = ComputeClipTransform(0x43F, 640, 640, -360, 360, 1280, 720, true);
  Project(h, -1, 1, 1, px, py);
  CHECK(px == Catch::Approx(0.5f).margin(1e-4));
  CHECK(py == Catch::Approx(0.5f).margin(1e-4));
}

TEST_CASE("pre-transformed positions land on their pixels") {
  // The game's full-screen passes: VTE scales off, positions in pixels.
  const ClipTransform t = ComputeClipTransform(0x400, 640, 640, -360, 360, 1280, 720, false);
  float px, py;
  for (const float x : {0.0f, 100.0f, 1280.0f}) {
    for (const float y : {0.0f, 333.0f, 720.0f}) {
      Project(t, x, y, 1, px, py);
      CHECK(px == Catch::Approx(x).margin(1e-3));
      CHECK(py == Catch::Approx(y).margin(1e-3));
    }
  }
  // Inside Vulkan's clip volume, so nothing is clipped.
  CHECK(1280 * t.ndc_scale[0] + t.offset[0] == Catch::Approx(1));
  CHECK(720 * t.ndc_scale[1] + t.offset[1] == Catch::Approx(-1));
  const ClipTransform h = ComputeClipTransform(0x400, 0, 0, 0, 0, 1280, 720, true);
  Project(h, 10, 20, 1, px, py);
  CHECK(px == Catch::Approx(10.5f).margin(1e-4));
  CHECK(py == Catch::Approx(20.5f).margin(1e-4));
}
