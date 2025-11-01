#ifndef VK_UTILS_H
#define VK_UTILS_H

#include <vulkan/vulkan_core.h>

VkCommandPoolCreateInfo
GetCommandPoolCreateInfo(uint32_t queueFamilyIndex,
						 VkCommandPoolCreateFlags flags);

VkCommandBufferAllocateInfo
GetCommandBufferAllocateInfo(VkCommandPool pool, uint32_t count);

VkFenceCreateInfo
GetFenceCreateInfo(VkFenceCreateFlags flags);

VkSemaphoreCreateInfo
GetSemaphoreCreateInfo(VkSemaphoreCreateFlags flags);

VkCommandBufferBeginInfo
GetCommandBufferBeginInfo(VkCommandBufferUsageFlags flags);

void TransitionImage(VkCommandBuffer buffer, VkImage image, VkImageLayout currentLayout, VkImageLayout nextLayout);
VkImageSubresourceRange GetImageSubresourceRange(VkImageAspectFlags aspectFlags);

VkSemaphoreSubmitInfo
GetSemaphoreSubmitInfo(VkPipelineStageFlags2 stageMask, VkSemaphore semaphore);

VkCommandBufferSubmitInfo
GetCommandBufferSubmitInfo(VkCommandBuffer cmd);

VkSubmitInfo2
GetSubmitInfo(VkCommandBufferSubmitInfo* cmd,
	VkSemaphoreSubmitInfo* signalSemaphoreInfo,
	VkSemaphoreSubmitInfo* waitSemaphoreInfo);

#endif
