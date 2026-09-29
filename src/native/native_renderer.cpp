#include "native/native_renderer.h"

#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/external_frame.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(svr_native_renderer, false, "SVR2011",
                    "Draw with the native Vulkan renderer instead of Xenos emulation "
                    "(experimental)");

namespace svr::native {

namespace {

using rex::ui::vulkan::VulkanDevice;
namespace vk_util = rex::ui::vulkan::util;

// The plugin samples a published frame after our submission, so three slots
// keep a frame we are writing apart from the one being presented.
constexpr uint32_t kFramesInFlight = 3;

struct FrameSlot {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};

struct State {
  const VulkanDevice* device = nullptr;
  FrameSlot slots[kFramesInFlight];
  uint32_t width = 1280;
  uint32_t height = 720;
  uint64_t frame = 0;
} g;

bool CreateFrameSlot(FrameSlot& slot) {
  const VulkanDevice::Functions& dfn = g.device->functions();
  const VkDevice device = g.device->device();

  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {g.width, g.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_util::CreateDedicatedAllocationImage(g.device, image_info,
                                               vk_util::MemoryPurpose::kDeviceLocal, slot.image,
                                               slot.memory)) {
    return false;
  }

  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = slot.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = vk_util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &slot.view) != VK_SUCCESS) {
    return false;
  }

  // One pool per slot, reset whole once the slot's fence has signalled.
  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = g.device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = slot.command_pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &allocate_info, &slot.command_buffer) != VK_SUCCESS) {
    return false;
  }

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  return dfn.vkCreateFence(device, &fence_info, nullptr, &slot.fence) == VK_SUCCESS;
}

bool CreateResources() {
  for (FrameSlot& slot : g.slots) {
    if (!CreateFrameSlot(slot)) {
      return false;
    }
  }
  return true;
}

void ImageBarrier(VkCommandBuffer command_buffer, VkImage image, VkPipelineStageFlags src_stage,
                  VkAccessFlags src_access, VkImageLayout old_layout,
                  VkPipelineStageFlags dst_stage, VkAccessFlags dst_access,
                  VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = vk_util::InitializeSubresourceRange();
  g.device->functions().vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr,
                                             0, nullptr, 1, &barrier);
}

}  // namespace

void Configure(rex::Runtime* runtime) {
  if (!REXCVAR_GET(svr_native_renderer)) {
    return;
  }
  auto* graphics_system = runtime ? runtime->graphics_system() : nullptr;
  // This build only has the Vulkan backend, so the provider is a VulkanProvider.
  auto* provider = graphics_system
                       ? static_cast<rex::ui::vulkan::VulkanProvider*>(graphics_system->provider())
                       : nullptr;
  g.device = provider ? provider->vulkan_device() : nullptr;
  if (!g.device || !CreateResources()) {
    REXLOG_ERROR("native renderer: could not create Vulkan resources; using Xenos emulation");
    g.device = nullptr;
    return;
  }
  rex::system::external_frame::SetEnabled(true);
  REXLOG_INFO("native renderer: enabled, output {}x{}", g.width, g.height);
}

void OnPresent() {
  if (!g.device) {
    return;
  }
  const VulkanDevice::Functions& dfn = g.device->functions();
  const VkDevice device = g.device->device();
  FrameSlot& slot = g.slots[g.frame % kFramesInFlight];

  dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device, 1, &slot.fence);
  dfn.vkResetCommandPool(device, slot.command_pool, 0);

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info);

  ImageBarrier(slot.command_buffer, slot.image, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // Milestone check: a slowly cycling colour proves the frame reaches the screen.
  const float t = float(g.frame % 360) * (6.2831853f / 360.0f);
  VkClearColorValue color = {};
  color.float32[0] = 0.5f + 0.5f * std::sin(t);
  color.float32[1] = 0.5f + 0.5f * std::sin(t + 2.094f);
  color.float32[2] = 0.5f + 0.5f * std::sin(t + 4.189f);
  color.float32[3] = 1.0f;
  const VkImageSubresourceRange range = vk_util::InitializeSubresourceRange();
  dfn.vkCmdClearColorImage(slot.command_buffer, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &color, 1, &range);
  // Later submissions on the same queue (the plugin's present) fall in the
  // barrier's second scope.
  ImageBarrier(slot.command_buffer, slot.image, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dfn.vkEndCommandBuffer(slot.command_buffer);

  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot.command_buffer;
  {
    VulkanDevice::Queue::Acquisition queue =
        g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, slot.fence) != VK_SUCCESS) {
      REXLOG_ERROR("native renderer: vkQueueSubmit failed");
      return;
    }
  }

  rex::system::external_frame::Frame frame;
  frame.image_view = uint64_t(reinterpret_cast<uintptr_t>(slot.view));
  frame.width = g.width;
  frame.height = g.height;
  frame.sequence = g.frame;
  rex::system::external_frame::Publish(frame);
  ++g.frame;
}

}  // namespace svr::native
