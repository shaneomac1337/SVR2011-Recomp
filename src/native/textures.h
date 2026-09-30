// Guest textures and samplers for the native renderer: fetch constants
// decoded into bindless image and sampler indices, and images that resolves
// write, standing in for the guest memory the emulator no longer fills.

#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace svr::native::textures {

// scale: the native resolution scale; resolve targets are that much larger
// than their guest textures.
bool Initialize(uint32_t scale);

// A texture fetch constant as the guest stores it (six big-endian dwords).
struct FetchConstant {
  uint32_t dwords[6];
};
FetchConstant LoadFetchConstant(const uint8_t* big_endian);

struct Binding {
  uint32_t texture_index = 0;  // in the heap matching the dimension
  uint32_t sampler_index = 0;
  uint32_t dimension = 1;  // xenos::DataDimension: 1 = 2D, 2 = 3D / stacked, 3 = cube
};

// Records guest-memory uploads into upload_cb when a texture is new or its
// data changed. Reading and converting guest data runs on worker threads;
// without wait, a texture still being read binds its previous content (or
// the default image when new), with wait the read is waited for.
// Unsupported textures bind the default image.
Binding Bind(const FetchConstant& fetch, VkCommandBuffer upload_cb, uint64_t frame, bool wait);

// Stops the worker threads; call before the device goes away.
void Shutdown();

// The image a resolve writes for a destination texture fetch constant,
// created on first use in TRANSFER_DST-compatible form. Later Binds of a
// fetch constant at the same address and format sample it.
struct ResolveTarget {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  VkImageLayout* layout = nullptr;
};
ResolveTarget GetResolveTarget(const FetchConstant& fetch, bool red_blue_swapped);

// Textures uploaded since the last call, and the CPU time checking them took.
struct UploadStats {
  uint64_t count;
  uint64_t bytes;
  uint64_t ns;        // render thread time on textures
  uint64_t wait_ns;   // of which waiting for workers
  uint64_t read_ns;   // reading guest data, any thread
  uint64_t watch_ns;  // of which write-protecting pages
  uint64_t hash_ns;   // of which hashing guest data
};
void TakeUploadStats(UploadStats& stats);

// Destroys guest-memory textures no draw has used for a while, so memory does
// not grow with every arena and attire the session loads.
void EvictUnused(uint64_t frame);

// The resolve target at a guest physical address, or an empty result.
ResolveTarget FindResolveTarget(uint32_t base_address);

// Bytes per pixel of a resolve destination's guest format, or 0 when its
// guest layout is not written back (compressed or unknown formats).
uint32_t GuestBytesPerPixel(const FetchConstant& fetch);

// Whether every byte of the format is an 8-bit unorm channel, so scaled
// readbacks can average bytes.
bool BytewiseUnorm(const FetchConstant& fetch);

// Writes resolved pixels (host order, tightly packed rows) into guest
// memory in the destination's layout: tiled or linear, endian-swapped.
void WriteToGuest(const FetchConstant& fetch, const uint8_t* pixels, uint32_t width,
                  uint32_t height);

}  // namespace svr::native::textures
