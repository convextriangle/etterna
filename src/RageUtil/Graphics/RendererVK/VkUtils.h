#ifndef VK_UTILS_H
#define VK_UTILS_H

#include <vulkan/vulkan_core.h>
#include <deque>
#include <functional>
#include <vk_mem_alloc.h>

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

struct DeletionQueue
{
	std::deque<std::function<void()>> Callbacks;
	void PushDeletionCallback(std::function<void()>&& callback);
	void FlushCallbacks();
};

struct AllocatedImage
{
	VkImage Image;
	VkImageView ImageView;
	VmaAllocation Allocation;
	VkExtent3D ImageExtent;
	VkFormat ImageFormat;
};

VkImageCreateInfo
GetImageCreateInfo(VkFormat format,
				   VkImageUsageFlags usageFlags,
				   VkExtent3D extent);

VkImageViewCreateInfo
GetImageViewCreateInfo(VkFormat format,
					   VkImage image,
					   VkImageAspectFlags aspectFlags);

void
CopyImageToImage(VkCommandBuffer buffer,
				 VkImage source,
				 VkImage dest,
				 VkExtent2D sourceSize,
				 VkExtent2D destSize);

#endif
