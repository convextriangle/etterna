#define VMA_IMPLEMENTATION
#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include "Core/Services/Locator.hpp"
#include <format>
#include <numbers>
#include <RageUtil/File/RageFileManager.h>

constexpr uint64_t Timeout = 1000'000'000;

std::string
RendererVK::GetApiDescription() const
{
	return "Vulkan";
}

void
RendererVK::InitializeRenderer(const VideoModeParams& p)
{
	InitVulkan();
	InitSwapchain(p);
	InitCommands();
	InitSyncStructures();

	InitInternalBuffers();
	InitBufferLayout();
	CreateDescriptorPool();
	CreateDescriptorSet();
	InitRenderPass();
	InitFramebuffers();
	InitGraphicsPipeline();
}

void
RendererVK::OnRender(const ActualVideoModeParams* p,
					 const Display::CommandBatcher& batcher)
{
	ThrowIfFail(vkWaitForFences(
	  m_Device, 1, &GetCurrentFrame().RenderFence, true, Timeout));
	ThrowIfFail(vkResetFences(m_Device, 1, &GetCurrentFrame().RenderFence));

	uint32_t swapchainImageIndex = 0;
	ThrowIfFail(vkAcquireNextImageKHR(m_Device,
									  m_Swapchain,
									  Timeout,
									  GetCurrentFrame().SwapchainSemaphore,
									  nullptr,
									  &swapchainImageIndex));

	auto buffer = GetCurrentFrame().MainCommandBuffer;

	ThrowIfFail(vkResetCommandBuffer(buffer, 0));
	auto beginInfo =
	  GetCommandBufferBeginInfo(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	ThrowIfFail(vkBeginCommandBuffer(buffer, &beginInfo));

	auto currentImage = m_SwapchainImages[swapchainImageIndex];

	VkRenderPassBeginInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = m_RenderPass;
	renderPassInfo.framebuffer = m_Framebuffers[swapchainImageIndex];
	renderPassInfo.renderArea.offset = { 0, 0 };
	renderPassInfo.renderArea.extent = m_SwapchainExtent;

	std::array<VkClearValue, 1> clearValues;
	clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	HandleDrawCommands(
	  buffer, currentImage, batcher.m_IndirectCommandBuffer.size(), p);

	vkCmdEndRenderPass(buffer);
	ThrowIfFail(vkEndCommandBuffer(buffer));

	auto bufferInfo = GetCommandBufferSubmitInfo(buffer);

	auto waitInfo =
	  GetSemaphoreSubmitInfo(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
							 GetCurrentFrame().SwapchainSemaphore);
	auto signalInfo = GetSemaphoreSubmitInfo(
	  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, GetCurrentFrame().RenderSemaphore);

	auto submitInfo = GetSubmitInfo(&bufferInfo, &signalInfo, &waitInfo);

	ThrowIfFail(vkQueueSubmit2(
	  m_GraphicsQueue, 1, &submitInfo, GetCurrentFrame().RenderFence));

	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &m_Swapchain;
	presentInfo.swapchainCount = 1;
	presentInfo.pWaitSemaphores = &GetCurrentFrame().RenderSemaphore;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pImageIndices = &swapchainImageIndex;

	ThrowIfFail(vkQueuePresentKHR(m_GraphicsQueue, &presentInfo));

	m_FrameNumber = (m_FrameNumber + 1) % FRAME_OVERLAP;
}

bool
RendererVK::IsD3DInternal()
{
	return false;
}

intptr_t
RendererVK::PushTextureCommand(const Display::TextureCommand& command)
{
	return intptr_t();
}

RendererVK::~RendererVK()
{
	vkDeviceWaitIdle(m_Device);
	for (size_t i = 0; i < FRAME_OVERLAP; i++) {
		vkDestroyCommandPool(m_Device, m_Frames[i].CommandPool, nullptr);
		vkDestroyFence(m_Device, m_Frames[i].RenderFence, nullptr);
		vkDestroySemaphore(m_Device, m_Frames[i].RenderSemaphore, nullptr);
		vkDestroySemaphore(m_Device, m_Frames[i].SwapchainSemaphore, nullptr);

		m_Frames[i].InfoDeletion.FlushCallbacks();
	}

	m_MainDeletionQueue.FlushCallbacks();

	DestroySwapchain();
	vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
	vkDestroyDevice(m_Device, nullptr);

	vkb::destroy_debug_utils_messenger(m_Instance, m_DebugMessenger);
	vkDestroyInstance(m_Instance, nullptr);
}

VkBool32
VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
					VkDebugUtilsMessageTypeFlagsEXT messageTypes,
					const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
					void* pUserData)
{
	Locator::getLogger()->warn("RendererVK debug callback: {}",
							   pCallbackData->pMessage);
	return VK_FALSE;
}

void
RendererVK::InitVulkan()
{
	vkb::InstanceBuilder builder;

	auto instanceResult =
	  builder.request_validation_layers(true)
		.use_default_debug_messenger()
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT)
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT)
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
		.set_debug_callback(VulkanDebugCallback)
		.require_api_version(1, 3, 0)
		.enable_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)
		.build();
	if (!instanceResult) {
		Fail();
	}

	m_Instance = instanceResult->instance;
	m_DebugMessenger = instanceResult->debug_messenger;

	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hwnd = GraphicsWindow::GetHwnd();
	createInfo.hinstance = GetModuleHandle(nullptr);
	ThrowIfFail(
	  vkCreateWin32SurfaceKHR(m_Instance, &createInfo, nullptr, &m_Surface));

	VkPhysicalDeviceVulkan13Features vk13Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
	};
	vk13Features.dynamicRendering = true;
	vk13Features.synchronization2 = true;

	VkPhysicalDeviceVulkan12Features vk12Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
	};
	vk12Features.bufferDeviceAddress = true;
	vk12Features.descriptorIndexing = true;
	vk12Features.runtimeDescriptorArray = true;

	VkPhysicalDeviceFeatures vkFeatures = {};
	vkFeatures.multiDrawIndirect = VK_TRUE;

	vkb::PhysicalDeviceSelector selector(*instanceResult);
	auto physicalDevice = selector.set_minimum_version(1, 3)
							.set_required_features_13(vk13Features)
							.set_required_features_12(vk12Features)
							.set_required_features(vkFeatures)
							.set_surface(m_Surface)
							.select();
	if (!physicalDevice) {
		Fail();
	}

	vkb::DeviceBuilder deviceBuilder(*physicalDevice);
	auto deviceResult = deviceBuilder.build();
	if (!deviceResult) {
		Fail();
	}

	m_Device = deviceResult->device;
	m_GPU = physicalDevice->physical_device;

	m_GraphicsQueue = deviceResult->get_queue(vkb::QueueType::graphics).value();
	m_GraphicsQueueFamily =
	  deviceResult->get_queue_index(vkb::QueueType::graphics).value();

	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = m_GPU;
	allocatorInfo.device = m_Device;
	allocatorInfo.instance = m_Instance;
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	ThrowIfFail(vmaCreateAllocator(&allocatorInfo, &m_Allocator));

	m_MainDeletionQueue.PushDeletionCallback(
	  [&]() { vmaDestroyAllocator(m_Allocator); });
}

void
RendererVK::InitSwapchain(const VideoModeParams& p)
{
	CreateSwapchain(p.width, p.height);
}

void
RendererVK::InitCommands()
{
	VkCommandPoolCreateInfo poolInfo = GetCommandPoolCreateInfo(
	  m_GraphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	for (size_t i = 0; i < FRAME_OVERLAP; i++) {
		ThrowIfFail(vkCreateCommandPool(
		  m_Device, &poolInfo, nullptr, &m_Frames[i].CommandPool));

		VkCommandBufferAllocateInfo bufferInfo =
		  GetCommandBufferAllocateInfo(m_Frames[i].CommandPool, 1);

		ThrowIfFail(vkAllocateCommandBuffers(
		  m_Device, &bufferInfo, &m_Frames[i].MainCommandBuffer));
	}
}

void
RendererVK::InitSyncStructures()
{
	VkFenceCreateInfo fenceInfo =
	  GetFenceCreateInfo(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphoreInfo = GetSemaphoreCreateInfo(0);

	for (size_t i = 0; i < FRAME_OVERLAP; i++) {
		ThrowIfFail(vkCreateFence(
		  m_Device, &fenceInfo, nullptr, &m_Frames[i].RenderFence));

		ThrowIfFail(vkCreateSemaphore(
		  m_Device, &semaphoreInfo, nullptr, &m_Frames[i].RenderSemaphore));
		ThrowIfFail(vkCreateSemaphore(
		  m_Device, &semaphoreInfo, nullptr, &m_Frames[i].SwapchainSemaphore));
	}
}

void
RendererVK::CreateSwapchain(size_t width, size_t height)
{
	vkb::SwapchainBuilder swapchainBuilder(m_GPU, m_Device, m_Surface);
	m_SwapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

	auto swapchainResult =
	  swapchainBuilder
		.set_desired_format(
		  VkSurfaceFormatKHR{ .format = m_SwapchainImageFormat,
							  .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
		.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
		.set_desired_extent(width, height)
		.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.build();

	if (!swapchainResult) {
		Fail();
	}

	m_SwapchainExtent = swapchainResult->extent;
	m_Swapchain = swapchainResult->swapchain;
	m_SwapchainImages = swapchainResult->get_images().value();
	m_SwapchainImageViews = swapchainResult->get_image_views().value();
}

void
RendererVK::DestroySwapchain()
{
	vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
	for (const auto& view : m_SwapchainImageViews) {
		vkDestroyImageView(m_Device, view, nullptr);
	}
}

FrameData&
RendererVK::GetCurrentFrame()
{
	return m_Frames[m_FrameNumber % FRAME_OVERLAP];
}

void
RendererVK::HandleDrawCommands(VkCommandBuffer buffer,
							   VkImage image,
							   uint32_t drawCount,
							   const ActualVideoModeParams* p)
{
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = p->width;
	viewport.height = p->height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(buffer, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = p->width;
	scissor.extent.height = p->height;

	vkCmdSetScissor(buffer, 0, 1, &scissor);

	vkCmdBindPipeline(
	  buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);

	VkBuffer vertexBuffers[] = { m_SpriteVertices };
	VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(buffer, 0, 1, vertexBuffers, offsets);

	vkCmdBindDescriptorSets(buffer,
							VK_PIPELINE_BIND_POINT_GRAPHICS,
							m_PipelineLayout,
							0,
							1,
							&m_BufferDescriptorSet,
							0,
							nullptr);

	if (drawCount > 0) {
		vkCmdDrawIndirect(buffer,
						  m_IndirectCommands,
						  0,
						  drawCount,
						  sizeof(Display::DrawCommand));
	}
}

void
RendererVK::InitBufferLayout()
{
	std::vector<VkDescriptorSetLayoutBinding> bindings = {
		{ .binding = 0,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1,
		  .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
		{ .binding = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1,
		  .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
		{ .binding = 2,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1,
		  .stageFlags = VK_SHADER_STAGE_VERTEX_BIT }
	};

	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(bindings.size()),
		.pBindings = bindings.data()
	};

	ThrowIfFail(vkCreateDescriptorSetLayout(
	  m_Device, &layoutInfo, nullptr, &m_BufferDescriptorLayout));
}

void
RendererVK::InitPipelineLayout()
{
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_BufferDescriptorLayout,
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr
	};

	ThrowIfFail(vkCreatePipelineLayout(
	  m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout));
}

void
RendererVK::CreateDescriptorPool()
{
	std::vector<VkDescriptorPoolSize> poolSizes = {
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 }
	};

	VkDescriptorPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
		.pPoolSizes = poolSizes.data()
	};

	ThrowIfFail(
	  vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool));
}

void
RendererVK::CreateDescriptorSet()
{
	VkDescriptorSetAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_DescriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &m_BufferDescriptorLayout
	};

	ThrowIfFail(
	  vkAllocateDescriptorSets(m_Device, &allocInfo, &m_BufferDescriptorSet));

	std::vector<VkDescriptorBufferInfo> bufferInfos = {
		{ .buffer = m_IndirectCommandArguments,
		  .offset = 0,
		  .range = VK_WHOLE_SIZE },
		{ .buffer = m_MatrixStates, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = m_RenderStates, .offset = 0, .range = VK_WHOLE_SIZE }
	};

	std::vector<VkWriteDescriptorSet> descriptorWrites = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		  .dstSet = m_BufferDescriptorSet,
		  .dstBinding = 0,
		  .dstArrayElement = 0,
		  .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &bufferInfos[0] },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		  .dstSet = m_BufferDescriptorSet,
		  .dstBinding = 1,
		  .dstArrayElement = 0,
		  .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &bufferInfos[1] },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		  .dstSet = m_BufferDescriptorSet,
		  .dstBinding = 2,
		  .dstArrayElement = 0,
		  .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &bufferInfos[2] }
	};

	vkUpdateDescriptorSets(m_Device,
						   static_cast<uint32_t>(descriptorWrites.size()),
						   descriptorWrites.data(),
						   0,
						   nullptr);
}

void
RendererVK::InitGraphicsPipeline()
{
	PipelineBuilder pipelineBuilder;

	InitPipelineLayout();
	pipelineBuilder.m_PipelineLayout = m_PipelineLayout;

	auto fragmentShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/fragment.glsl"),
	  m_Device,
	  shaderc_glsl_fragment_shader);
	auto vertexShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/vertex.glsl"),
	  m_Device,
	  shaderc_glsl_vertex_shader);

	pipelineBuilder.SetShaders(vertexShader, fragmentShader);
	pipelineBuilder.SetInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.SetPolygonMode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.SetCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.DisableMultisampling();
	pipelineBuilder.DisableBlending();
	pipelineBuilder.DisableDepthTest();

	pipelineBuilder.SetColorAttachmentFormat(m_SwapchainImageFormat);
	pipelineBuilder.SetDepthFormat(VK_FORMAT_UNDEFINED);

	pipelineBuilder.m_VertexInfo = GetSpriteVertexInfo();

	m_GraphicsPipeline = pipelineBuilder.BuildPipeline(m_Device, m_RenderPass);

	vkDestroyShaderModule(m_Device, fragmentShader, nullptr);
	vkDestroyShaderModule(m_Device, vertexShader, nullptr);

	m_MainDeletionQueue.PushDeletionCallback([&]() {
		vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
		vkDestroyPipeline(m_Device, m_GraphicsPipeline, nullptr);
	});
}

VkPipelineVertexInputStateCreateInfo
RendererVK::GetSpriteVertexInfo()
{
	static const std::vector<VkVertexInputBindingDescription>
	  bindingDescriptions = { { .binding = 0,
								.stride = sizeof(RageSpriteVertex),
								.inputRate = VK_VERTEX_INPUT_RATE_VERTEX } };

	static const std::vector<VkVertexInputAttributeDescription>
	  attributeDescriptions = { { .location = 0,
								  .binding = 0,
								  .format = VK_FORMAT_R32G32B32_SFLOAT,
								  .offset = offsetof(RageSpriteVertex, p) },
								{ .location = 1,
								  .binding = 0,
								  .format = VK_FORMAT_R32G32B32_SFLOAT,
								  .offset = offsetof(RageSpriteVertex, n) },
								{ .location = 2,
								  .binding = 0,
								  .format = VK_FORMAT_R32G32B32A32_SFLOAT,
								  .offset = offsetof(RageSpriteVertex, c) },
								{ .location = 3,
								  .binding = 0,
								  .format = VK_FORMAT_R32G32_SFLOAT,
								  .offset = offsetof(RageSpriteVertex, t) } };

	VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount =
		  static_cast<uint32_t>(bindingDescriptions.size()),
		.pVertexBindingDescriptions = bindingDescriptions.data(),
		.vertexAttributeDescriptionCount =
		  static_cast<uint32_t>(attributeDescriptions.size()),
		.pVertexAttributeDescriptions = attributeDescriptions.data()
	};

	return vertexInputInfo;
}

void
RendererVK::InitInternalBuffers()
{
	constexpr size_t maxCommands = 20'000;

	CreateDynamicBuffer(m_Device,
						m_GPU,
						m_IndirectCommands,
						m_IndirectCommandMemory,
						maxCommands * sizeof(Display::DrawCommand),
						VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
	CreateDynamicBuffer(m_Device,
						m_GPU,
						m_IndirectCommandArguments,
						m_IndirectCommandArgumentMemory,
						maxCommands * sizeof(Display::DrawCommandArgument),
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	CreateDynamicBuffer(m_Device,
						m_GPU,
						m_SpriteVertices,
						m_SpriteVertexMemory,
						maxCommands * sizeof(RageSpriteVertex),
						VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	CreateDynamicBuffer(m_Device,
						m_GPU,
						m_MatrixStates,
						m_MatrixStateMemory,
						maxCommands * sizeof(Display::MatrixState),
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	CreateDynamicBuffer(m_Device,
						m_GPU,
						m_RenderStates,
						m_RenderStateMemory,
						maxCommands * sizeof(Display::RenderState),
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	m_MainDeletionQueue.PushDeletionCallback([&]() {
		vkDestroyBuffer(m_Device, m_IndirectCommands, nullptr);
		vkDestroyBuffer(m_Device, m_IndirectCommandArguments, nullptr);
		vkDestroyBuffer(m_Device, m_SpriteVertices, nullptr);
		vkDestroyBuffer(m_Device, m_MatrixStates, nullptr);
		vkDestroyBuffer(m_Device, m_RenderStates, nullptr);

		vkFreeMemory(m_Device, m_IndirectCommandMemory, nullptr);
		vkFreeMemory(m_Device, m_IndirectCommandArgumentMemory, nullptr);
		vkFreeMemory(m_Device, m_SpriteVertexMemory, nullptr);
		vkFreeMemory(m_Device, m_MatrixStateMemory, nullptr);
		vkFreeMemory(m_Device, m_RenderStateMemory, nullptr);
	});
}

void
RendererVK::UpdateInternalBuffers(const Display::CommandBatcher& batcher)
{
	UpdateDynamicBuffer(m_Device,
						m_GPU,
						m_IndirectCommands,
						m_IndirectCommandMemory,
						batcher.m_IndirectCommandBuffer.data(),
						batcher.m_IndirectCommandBuffer.size() *
						  sizeof(Display::DrawCommand));
	UpdateDynamicBuffer(m_Device,
						m_GPU,
						m_IndirectCommandArguments,
						m_IndirectCommandArgumentMemory,
						batcher.m_IndirectCommandArgumentBuffer.data(),
						batcher.m_IndirectCommandArgumentBuffer.size() *
						  sizeof(Display::DrawCommandArgument));
	UpdateDynamicBuffer(m_Device,
						m_GPU,
						m_SpriteVertices,
						m_SpriteVertexMemory,
						batcher.m_SpriteVertexBuffer.data(),
						batcher.m_SpriteVertexBuffer.size() *
						  sizeof(Display::DrawCommand));
	UpdateDynamicBuffer(m_Device,
						m_GPU,
						m_MatrixStates,
						m_MatrixStateMemory,
						batcher.m_MatrixStateBuffer.data(),
						batcher.m_MatrixStateBuffer.size() *
						  sizeof(Display::DrawCommand));
	UpdateDynamicBuffer(m_Device,
						m_GPU,
						m_RenderStates,
						m_RenderStateMemory,
						batcher.m_RenderStateBuffer.data(),
						batcher.m_RenderStateBuffer.size() *
						  sizeof(Display::DrawCommand));
}

void
RendererVK::InitRenderPass()
{
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = m_SwapchainImageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference attachmentRef = {};
	attachmentRef.attachment = 0;
	attachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &attachmentRef;

	VkRenderPassCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &colorAttachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.pDependencies = &dependency;
	info.dependencyCount = 1;

	ThrowIfFail(vkCreateRenderPass(m_Device, &info, nullptr, &m_RenderPass));
}

void
RendererVK::InitFramebuffers()
{
	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.pNext = nullptr;
	framebufferInfo.renderPass = m_RenderPass;
	framebufferInfo.attachmentCount = 1;
	framebufferInfo.width = m_SwapchainExtent.width;
	framebufferInfo.height = m_SwapchainExtent.height;
	framebufferInfo.layers = 1;

	const uint32_t imageCount = m_SwapchainImages.size();
	m_Framebuffers = std::vector<VkFramebuffer>(imageCount);

	for (int i = 0; i < imageCount; i++) {

		framebufferInfo.pAttachments = &m_SwapchainImageViews[i];
		ThrowIfFail(vkCreateFramebuffer(
		  m_Device, &framebufferInfo, nullptr, &m_Framebuffers[i]));
	}

	m_MainDeletionQueue.PushDeletionCallback([&]() {
		vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
		for (int i = 0; i < imageCount; i++) {
			vkDestroyFramebuffer(m_Device, m_Framebuffers[i], nullptr);
		}
	});
}
