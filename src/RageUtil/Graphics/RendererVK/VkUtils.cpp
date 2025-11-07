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
ThrowIfFail(VkResult result, const std::source_location location)
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
LoadShaderFromFile(std::string path,
				   VkDevice device,
				   shaderc_shader_kind shaderKind)
{
#ifdef _WIN32
	if (path[0] == '/') {
		path = path.substr(1);
	}
#endif

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

uint32_t
FindMemoryType(VkPhysicalDevice physicalDevice,
			   uint32_t typeFilter,
			   VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memoryProps = {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProps);

	for (uint32_t i = 0; i < memoryProps.memoryTypeCount; i++) {
		if ((typeFilter & (1 << i)) &&
			(memoryProps.memoryTypes[i].propertyFlags & properties) ==
			  properties) {
			return i;
		}
	}

	Fail();
}

void
CreateBuffer(VkDevice device,
			 VkPhysicalDevice gpu,
			 VkDeviceSize size,
			 VkBufferUsageFlags usageFlags,
			 VkMemoryPropertyFlags properties,
			 VkBuffer& buffer,
			 VkDeviceMemory& bufferMemory)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usageFlags;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	ThrowIfFail(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex =
	  FindMemoryType(gpu, requirements.memoryTypeBits, properties);

	ThrowIfFail(vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory));

	ThrowIfFail(vkBindBufferMemory(device, buffer, bufferMemory, 0));
}

void
CreateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					size_t neededSize)
{
	CreateBuffer(device,
				 gpu,
				 neededSize,
				 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				 buffer,
				 bufferMemory);
}

void
UpdateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					const void* data,
					size_t dataSize)
{
	void* mappedData = nullptr;
	ThrowIfFail(vkMapMemory(device, bufferMemory, 0, dataSize, 0, &mappedData));

	std::memcpy(mappedData, data, dataSize);

	vkUnmapMemory(device, bufferMemory);
}

VkPipelineShaderStageCreateInfo
GetShaderStageCreateInfo(VkShaderStageFlagBits stage,
						 VkShaderModule shaderModule)
{
	VkPipelineShaderStageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.pNext = nullptr;

	info.stage = stage;
	info.module = shaderModule;
	info.pName = "main";
	return info;
}

VkPipeline
PipelineBuilder::BuildPipeline(VkDevice device)
{
	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.pNext = nullptr;

	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT,
												  VK_DYNAMIC_STATE_SCISSOR };

	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount =
	  static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineColorBlendStateCreateInfo colorBlending = {};
	colorBlending.sType =
	  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.pNext = nullptr;

	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &m_ColorBlendAttachment;

	VkGraphicsPipelineCreateInfo pipelineInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
	};
	pipelineInfo.pNext = &m_RenderInfo;

	pipelineInfo.stageCount = (uint32_t)m_ShaderStages.size();
	pipelineInfo.pStages = m_ShaderStages.data();
	pipelineInfo.pVertexInputState = &m_VertexInfo;
	pipelineInfo.pInputAssemblyState = &m_InputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pRasterizationState = &m_Rasterizer;
	pipelineInfo.pMultisampleState = &m_Multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDepthStencilState = &m_DepthStencil;
	pipelineInfo.layout = m_PipelineLayout;

	VkPipeline newPipeline = VK_NULL_HANDLE;
	ThrowIfFail(vkCreateGraphicsPipelines(
	  device, nullptr, 1, &pipelineInfo, nullptr, &newPipeline));

	return newPipeline;
}

void
PipelineBuilder::SetShaders(VkShaderModule vertexShader,
							VkShaderModule fragmentShader)
{
	m_ShaderStages.clear();

	m_ShaderStages.push_back(
	  GetShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, vertexShader));
	m_ShaderStages.push_back(
	  GetShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader));
}

void
PipelineBuilder::SetInputTopology(VkPrimitiveTopology topology)
{
	m_InputAssembly.topology = topology;
	m_InputAssembly.primitiveRestartEnable = VK_FALSE;
}

void
PipelineBuilder::SetPolygonMode(VkPolygonMode mode)
{
	m_Rasterizer.polygonMode = mode;
	m_Rasterizer.lineWidth = 1.f;
}

void
PipelineBuilder::SetCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace)
{
	m_Rasterizer.frontFace = frontFace;
	m_Rasterizer.cullMode = cullMode;
}

void
PipelineBuilder::DisableMultisampling()
{
	m_Multisampling.sampleShadingEnable = VK_FALSE;
	m_Multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	m_Multisampling.minSampleShading = 1.0f;
	m_Multisampling.pSampleMask = nullptr;
	m_Multisampling.alphaToCoverageEnable = VK_FALSE;
	m_Multisampling.alphaToOneEnable = VK_FALSE;
}

void
PipelineBuilder::DisableBlending()
{
	m_ColorBlendAttachment.colorWriteMask =
	  VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	m_ColorBlendAttachment.blendEnable = VK_FALSE;
}

void
PipelineBuilder::SetColorAttachmentFormat(VkFormat format)
{
	m_ColorAttachmentFormat = format;
	m_RenderInfo.colorAttachmentCount = 1;
	m_RenderInfo.pColorAttachmentFormats = &m_ColorAttachmentFormat;
}

void
PipelineBuilder::SetDepthFormat(VkFormat format)
{
	m_RenderInfo.depthAttachmentFormat = format;
}

void
PipelineBuilder::DisableDepthTest()
{
	m_DepthStencil.depthTestEnable = VK_FALSE;
	m_DepthStencil.depthWriteEnable = VK_FALSE;
	m_DepthStencil.depthCompareOp = VK_COMPARE_OP_NEVER;
	m_DepthStencil.depthBoundsTestEnable = VK_FALSE;
	m_DepthStencil.stencilTestEnable = VK_FALSE;
	m_DepthStencil.front = {};
	m_DepthStencil.back = {};
	m_DepthStencil.minDepthBounds = 0.f;
	m_DepthStencil.maxDepthBounds = 1.f;
}

void
PipelineBuilder::Clear()
{
	m_InputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
	};

	m_Rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
	};

	m_ColorBlendAttachment = {};

	m_Multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
	};

	m_PipelineLayout = {};

	m_DepthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
	};

	m_RenderInfo = { .sType =
					   VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };

	m_ShaderStages.clear();
}
