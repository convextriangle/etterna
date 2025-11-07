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

void
TransitionImage(VkCommandBuffer buffer,
				VkImage image,
				VkImageLayout currentLayout,
				VkImageLayout nextLayout);
VkImageSubresourceRange
GetImageSubresourceRange(VkImageAspectFlags aspectFlags);

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

void
ThrowIfFail(
  VkResult result,
  const std::source_location location = std::source_location::current());

void
Fail(const std::source_location location = std::source_location::current());

std::vector<uint32_t>
CompileShader(const std::string& sourceName,
			  shaderc_shader_kind shaderKind,
			  const std::string& source);

VkShaderModule
LoadShaderFromFile(std::string path,
				   VkDevice device,
				   shaderc_shader_kind shaderKind);

void
CreateBuffer(VkDevice device,
			 VkPhysicalDevice gpu,
			 VkDeviceSize size,
			 VkBufferUsageFlags usageFlags,
			 VkMemoryPropertyFlags properties,
			 VkBuffer& buffer,
			 VkDeviceMemory& bufferMemory);

void
CreateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					size_t neededSize);

void
UpdateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					const void* data,
					size_t dataSize);

VkPipelineShaderStageCreateInfo
GetShaderStageCreateInfo(VkShaderStageFlagBits stage,
						 VkShaderModule shaderModule);

class PipelineBuilder
{
  public:
	std::vector<VkPipelineShaderStageCreateInfo> m_ShaderStages;

	VkPipelineInputAssemblyStateCreateInfo m_InputAssembly;
	VkPipelineRasterizationStateCreateInfo m_Rasterizer;
	VkPipelineColorBlendAttachmentState m_ColorBlendAttachment;
	VkPipelineMultisampleStateCreateInfo m_Multisampling;
	VkPipelineLayout m_PipelineLayout;
	VkPipelineDepthStencilStateCreateInfo m_DepthStencil;
	VkPipelineRenderingCreateInfo m_RenderInfo;
	VkFormat m_ColorAttachmentFormat;
	VkPipelineVertexInputStateCreateInfo m_VertexInfo;

	PipelineBuilder() { Clear(); }
	void Clear();
	VkPipeline BuildPipeline(VkDevice device);
	void SetShaders(VkShaderModule vertexShader, VkShaderModule fragmentShader);
	void SetInputTopology(VkPrimitiveTopology topology);
	void SetPolygonMode(VkPolygonMode mode);
	void SetCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace);
	void DisableMultisampling();
	void DisableBlending();
	void SetColorAttachmentFormat(VkFormat format);
	void SetDepthFormat(VkFormat format);
	void DisableDepthTest();
};

#endif
