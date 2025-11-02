#ifndef VK_UTILS_H
#define VK_UTILS_H

#include <vulkan/vulkan.h>
#include <deque>
#include <functional>
#include <vk_mem_alloc.h>
#include <source_location>
#include <span>
#include <shaderc/shaderc.hpp>

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

struct DescriptorLayoutBuilder
{
	std::vector<VkDescriptorSetLayoutBinding> Bindings;
	void AddBinding(uint32_t binding, VkDescriptorType type);
	void Clear();
	VkDescriptorSetLayout Build(VkDevice device,
								VkShaderStageFlags shaderStages,
								void* pNext = nullptr,
								VkDescriptorSetLayoutCreateFlags flags = 0);
};

void
ThrowIfFail(
  VkResult result,
  const std::source_location location = std::source_location::current());

void
Fail(const std::source_location location = std::source_location::current());

struct DescriptorAllocator
{
	struct PoolSizeRatio
	{
		VkDescriptorType type;
		float ratio;
	};

	VkDescriptorPool Pool;
	void InitPool(VkDevice device,
				  uint32_t maxSets,
				  std::span<PoolSizeRatio> poolRatios);
	void DestroyPool(VkDevice device);
	void ClearDescriptors(VkDevice device);
	VkDescriptorSet Allocate(VkDevice device, VkDescriptorSetLayout layout);
};

std::vector<uint32_t>
CompileShader(const std::string& sourceName,
			  shaderc_shader_kind shaderKind,
			  const std::string& source);

VkShaderModule
LoadShaderFromFile(const std::string& path,
				   VkDevice device,
				   shaderc_shader_kind shaderKind);

#endif
