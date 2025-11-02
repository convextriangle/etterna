#include "VkUtils.h"
#include <format>
#include <fstream>
#include <sstream>
#include "Core/Services/Locator.hpp"

VkCommandPoolCreateInfo
GetCommandPoolCreateInfo(uint32_t queueFamilyIndex,
						 VkCommandPoolCreateFlags flags)
{
	VkCommandPoolCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	info.pNext = nullptr;
	info.queueFamilyIndex = queueFamilyIndex;
	info.flags = flags;
	return info;
}

VkCommandBufferAllocateInfo
GetCommandBufferAllocateInfo(VkCommandPool pool, uint32_t count)
{
	VkCommandBufferAllocateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	info.pNext = nullptr;

	info.commandPool = pool;
	info.commandBufferCount = count;
	info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	return info;
}

VkFenceCreateInfo
GetFenceCreateInfo(VkFenceCreateFlags flags)
{
	VkFenceCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	info.pNext = nullptr;

	info.flags = flags;

	return info;
}

VkSemaphoreCreateInfo
GetSemaphoreCreateInfo(VkSemaphoreCreateFlags flags)
{
	VkSemaphoreCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	info.pNext = nullptr;
	info.flags = flags;
	return info;
}

VkCommandBufferBeginInfo
GetCommandBufferBeginInfo(VkCommandBufferUsageFlags flags)
{
	VkCommandBufferBeginInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.pNext = nullptr;

	info.pInheritanceInfo = nullptr;
	info.flags = flags;
	return info;
}

void
TransitionImage(VkCommandBuffer cmd,
				VkImage image,
				VkImageLayout currentLayout,
				VkImageLayout nextLayout)
{
	VkImageMemoryBarrier2 imageBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
	};
	imageBarrier.pNext = nullptr;

	imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	imageBarrier.dstAccessMask =
	  VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

	imageBarrier.oldLayout = currentLayout;
	imageBarrier.newLayout = nextLayout;

	VkImageAspectFlags aspectFlags =
	  (nextLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
		? VK_IMAGE_ASPECT_DEPTH_BIT
		: VK_IMAGE_ASPECT_COLOR_BIT;
	imageBarrier.subresourceRange = GetImageSubresourceRange(aspectFlags);
	imageBarrier.image = image;

	VkDependencyInfo depInfo{};
	depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	depInfo.pNext = nullptr;

	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &imageBarrier;

	vkCmdPipelineBarrier2(cmd, &depInfo);
}

VkImageSubresourceRange
GetImageSubresourceRange(VkImageAspectFlags aspectFlags)
{
	VkImageSubresourceRange subImage = {};
	subImage.aspectMask = aspectFlags;
	subImage.baseMipLevel = 0;
	subImage.levelCount = VK_REMAINING_MIP_LEVELS;
	subImage.baseArrayLayer = 0;
	subImage.layerCount = VK_REMAINING_ARRAY_LAYERS;

	return subImage;
}

VkSemaphoreSubmitInfo
GetSemaphoreSubmitInfo(VkPipelineStageFlags2 stageMask, VkSemaphore semaphore)
{
	VkSemaphoreSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	submitInfo.pNext = nullptr;
	submitInfo.semaphore = semaphore;
	submitInfo.stageMask = stageMask;
	submitInfo.deviceIndex = 0;
	submitInfo.value = 1;

	return submitInfo;
}

VkCommandBufferSubmitInfo
GetCommandBufferSubmitInfo(VkCommandBuffer cmd)
{
	VkCommandBufferSubmitInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	info.pNext = nullptr;
	info.commandBuffer = cmd;
	info.deviceMask = 0;

	return info;
}

VkSubmitInfo2
GetSubmitInfo(VkCommandBufferSubmitInfo* cmd,
			  VkSemaphoreSubmitInfo* signalSemaphoreInfo,
			  VkSemaphoreSubmitInfo* waitSemaphoreInfo)
{
	VkSubmitInfo2 info = {};
	info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	info.pNext = nullptr;

	info.waitSemaphoreInfoCount = waitSemaphoreInfo == nullptr ? 0 : 1;
	info.pWaitSemaphoreInfos = waitSemaphoreInfo;

	info.signalSemaphoreInfoCount = signalSemaphoreInfo == nullptr ? 0 : 1;
	info.pSignalSemaphoreInfos = signalSemaphoreInfo;

	info.commandBufferInfoCount = 1;
	info.pCommandBufferInfos = cmd;

	return info;
}

VkImageCreateInfo
GetImageCreateInfo(VkFormat format,
				   VkImageUsageFlags usageFlags,
				   VkExtent3D extent)
{
	VkImageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = nullptr;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = extent;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usageFlags;

	return info;
}

VkImageViewCreateInfo
GetImageViewCreateInfo(VkFormat format,
					   VkImage image,
					   VkImageAspectFlags aspectFlags)
{
	VkImageViewCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.pNext = nullptr;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.image = image;
	info.format = format;
	info.subresourceRange.baseMipLevel = 0;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.baseArrayLayer = 0;
	info.subresourceRange.layerCount = 1;
	info.subresourceRange.aspectMask = aspectFlags;

	return info;
}

void
CopyImageToImage(VkCommandBuffer buffer,
				 VkImage source,
				 VkImage dest,
				 VkExtent2D sourceSize,
				 VkExtent2D destSize)
{
	VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
							 .pNext = nullptr };

	blitRegion.srcOffsets[1].x = sourceSize.width;
	blitRegion.srcOffsets[1].y = sourceSize.height;
	blitRegion.srcOffsets[1].z = 1;

	blitRegion.dstOffsets[1].x = destSize.width;
	blitRegion.dstOffsets[1].y = destSize.height;
	blitRegion.dstOffsets[1].z = 1;

	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcSubresource.mipLevel = 0;

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstSubresource.mipLevel = 0;

	VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
							   .pNext = nullptr };
	blitInfo.dstImage = dest;
	blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	blitInfo.srcImage = source;
	blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	blitInfo.filter = VK_FILTER_LINEAR;
	blitInfo.regionCount = 1;
	blitInfo.pRegions = &blitRegion;

	vkCmdBlitImage2(buffer, &blitInfo);
}

void
DeletionQueue::PushDeletionCallback(std::function<void()>&& callback)
{
	Callbacks.push_back(callback);
}

void
DeletionQueue::FlushCallbacks()
{
	for (auto it = Callbacks.rbegin(); it != Callbacks.rend(); it++) {
		(*it)();
	}

	Callbacks.clear();
}

void
DescriptorLayoutBuilder::AddBinding(uint32_t binding, VkDescriptorType type)
{
	VkDescriptorSetLayoutBinding bind{};
	bind.binding = binding;
	bind.descriptorCount = 1;
	bind.descriptorType = type;

	Bindings.push_back(bind);
}

void
DescriptorLayoutBuilder::Clear()
{
	Bindings.clear();
}

VkDescriptorSetLayout
DescriptorLayoutBuilder::Build(VkDevice device,
							   VkShaderStageFlags shaderStages,
							   void* pNext,
							   VkDescriptorSetLayoutCreateFlags flags)
{
	for (auto& binding : Bindings) {
		binding.stageFlags |= shaderStages;
	}

	VkDescriptorSetLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
	};
	info.pNext = pNext;
	info.pBindings = Bindings.data();
	info.bindingCount = Bindings.size();
	info.flags = flags;

	VkDescriptorSetLayout set;
	ThrowIfFail(vkCreateDescriptorSetLayout(device, &info, nullptr, &set));

	return set;
}

void
ThrowIfFail(
  VkResult result,
  const std::source_location location)
{
	if (result == VK_SUCCESS) {
		return;
	}

	const std::string message =
	  std::format("RendererVK failed: VkResult {} at {}:{} in function {}",
				  static_cast<int>(result),
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

void
Fail(const std::source_location location)
{
	const std::string message =
	  std::format("RendererVK failed at {}:{} in function {}",
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

std::vector<uint32_t>
CompileShader(const std::string& sourceName,
			  shaderc_shader_kind shaderKind,
			  const std::string& source)
{
	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	auto result = compiler.CompileGlslToSpv(
	  source, shaderKind, sourceName.c_str(), options);

	if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
		auto message = std::format("Vulkan GLSL shader compilation failed: {}",
								   result.GetErrorMessage());
		throw std::runtime_error(message);
	}

	return { result.begin(), result.end() };
}

VkShaderModule
LoadShaderFromFile(const std::string& path,
				   VkDevice device,
				   shaderc_shader_kind shaderKind)
{
	std::ifstream inputFile(path);
	std::stringstream contents;
	contents << inputFile.rdbuf();
	auto shaderBlob = CompileShader("meow", shaderKind, contents.str());

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.pNext = nullptr;
	createInfo.codeSize = shaderBlob.size() * sizeof(uint32_t);
	createInfo.pCode = shaderBlob.data();

	VkShaderModule result = {};
	ThrowIfFail(vkCreateShaderModule(device, &createInfo, nullptr, &result));

	return result;
}

void
DescriptorAllocator::InitPool(VkDevice device,
							  uint32_t maxSets,
							  std::span<PoolSizeRatio> poolRatios)
{
	std::vector<VkDescriptorPoolSize> poolSizes;
	for (PoolSizeRatio ratio : poolRatios) {
		poolSizes.push_back(VkDescriptorPoolSize{
		  .type = ratio.type,
		  .descriptorCount = uint32_t(ratio.ratio * maxSets) });
	}

	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
	};
	pool_info.flags = 0;
	pool_info.maxSets = maxSets;
	pool_info.poolSizeCount = poolSizes.size();
	pool_info.pPoolSizes = poolSizes.data();

	ThrowIfFail(vkCreateDescriptorPool(device, &pool_info, nullptr, &Pool));
}

void
DescriptorAllocator::DestroyPool(VkDevice device)
{
	ThrowIfFail(vkResetDescriptorPool(device, Pool, 0));
}

void
DescriptorAllocator::ClearDescriptors(VkDevice device)
{
	vkDestroyDescriptorPool(device, Pool, nullptr);
}

VkDescriptorSet
DescriptorAllocator::Allocate(VkDevice device, VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
	};
	allocInfo.pNext = nullptr;
	allocInfo.descriptorPool = Pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet set;
	ThrowIfFail(vkAllocateDescriptorSets(device, &allocInfo, &set));

	return set;
}
