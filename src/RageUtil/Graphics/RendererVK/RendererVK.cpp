#define VMA_IMPLEMENTATION
#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include "Core/Services/Locator.hpp"
#include <format>
#include <numbers>
#include <RageUtil/File/RageFileManager.h>
#include <RageUtil/Misc/RageMath.h>

constexpr uint64_t Timeout = 1000'000'000;

std::string
RendererVK::GetApiDescription() const
{
	return "Vulkan";
}

void
RendererVK::InitializeRenderer(const VideoModeParams& p)
{
	InitVulkanState();
	InitSwapchain(p);
	InitImageViews();
	InitGraphicsPipeline();
	InitCommandPool();
	InitBatchBuffers();
	InitCommandBuffers();
	InitSyncStructures();
}

/// ----------------------------------------
/// here be hazards and unsignaled fences...
/// ----------------------------------------
void
RendererVK::OnRender(const ActualVideoModeParams* p,
					 const Display::CommandBatcher& batcher)
{
	ThrowIfFail(m_Device.waitForFences(
	  *m_InFlightFence[currentFrame], vk::True, Timeout));

	UpdateBatchBuffers(batcher);

	auto [result, imageIndex] = m_Swapchain.acquireNextImage(
	  Timeout, *m_PresentCompleteSemaphore[semaphoreIndex], nullptr);

	if (result == vk::Result::eErrorOutOfDateKHR) {
		RecreateSwapchain(*p);
		return;
	}
	ThrowIfFail(result);

	m_Device.resetFences(*m_InFlightFence[currentFrame]);
	m_CommandBuffers[currentFrame].reset();
	RecordCommands(imageIndex, batcher.m_IndirectCommandBuffer.size());

	vk::PipelineStageFlags waitDestinationStageMask(
	  vk::PipelineStageFlagBits::eColorAttachmentOutput);

	vk::SubmitInfo submitInfo{};
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &*m_PresentCompleteSemaphore[semaphoreIndex];
	submitInfo.pWaitDstStageMask = &waitDestinationStageMask;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &*m_CommandBuffers[currentFrame];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &*m_RenderFinishedSemaphore[imageIndex];
	m_GraphicsQueue.submit(submitInfo, *m_InFlightFence[currentFrame]);

	vk::PresentInfoKHR presentInfoKHR{};
	presentInfoKHR.waitSemaphoreCount = 1;
	presentInfoKHR.pWaitSemaphores = &*m_RenderFinishedSemaphore[imageIndex];
	presentInfoKHR.swapchainCount = 1;
	presentInfoKHR.pSwapchains = &*m_Swapchain;
	presentInfoKHR.pImageIndices = &imageIndex;

	result = m_PresentQueue.presentKHR(presentInfoKHR);
	if (result == vk::Result::eErrorOutOfDateKHR ||
		result == vk::Result::eSuboptimalKHR) {
		RecreateSwapchain(*p);
		return;
	}
	ThrowIfFail(result);

	semaphoreIndex = (semaphoreIndex + 1) % m_PresentCompleteSemaphore.size();
	currentFrame = (currentFrame + 1) % FramesInFlight;
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
	// WHAT
	vkDeviceWaitIdle(static_cast<VkDevice>(static_cast<vk::Device>(m_Device)));
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
RendererVK::InitVulkanState()
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

	m_Instance = vk::raii::Instance(m_Context, instanceResult->instance);
	m_DebugMessenger = vk::raii::DebugUtilsMessengerEXT(
	  m_Instance, instanceResult->debug_messenger);

	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hwnd = GraphicsWindow::GetHwnd();
	createInfo.hinstance = GetModuleHandle(nullptr);
	m_Surface = m_Instance.createWin32SurfaceKHR(createInfo);

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
	vk12Features.drawIndirectCount = true;

	VkPhysicalDeviceVulkan11Features vk11Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
	};
	vk11Features.shaderDrawParameters = true;

	VkPhysicalDeviceFeatures vkFeatures = {};
	vkFeatures.multiDrawIndirect = vk::True;
	vkFeatures.logicOp = vk::True;
	vkFeatures.drawIndirectFirstInstance = vk::True;

	vkb::PhysicalDeviceSelector selector(*instanceResult);
	auto physicalDeviceResult =
	  selector.set_minimum_version(1, 3)
		.set_required_features_13(vk13Features)
		.set_required_features_12(vk12Features)
		.set_required_features_11(vk11Features)
		.set_required_features(vkFeatures)
		.set_surface(static_cast<vk::SurfaceKHR>(m_Surface))
		.select();
	if (!physicalDeviceResult) {
		Fail();
	}

	vkb::DeviceBuilder deviceBuilder(*physicalDeviceResult);
	auto deviceResult = deviceBuilder.build();
	if (!deviceResult) {
		Fail();
	}

	m_PhysicalDevice = vk::raii::PhysicalDevice(
	  m_Instance, physicalDeviceResult->physical_device);
	m_Device = vk::raii::Device(m_PhysicalDevice, deviceResult->device);

	m_GraphicsQueue = vk::raii::Queue(
	  m_Device, deviceResult->get_queue(vkb::QueueType::graphics).value());
	m_GraphicsQueueFamily =
	  deviceResult->get_queue_index(vkb::QueueType::graphics).value();

	m_PresentQueue = vk::raii::Queue(
	  m_Device, deviceResult->get_queue(vkb::QueueType::present).value());
	m_PresentQueueFamily =
	  deviceResult->get_queue_index(vkb::QueueType::present).value();

	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice =
	  static_cast<vk::PhysicalDevice>(m_PhysicalDevice);
	allocatorInfo.device = static_cast<vk::Device>(m_Device);
	allocatorInfo.instance = static_cast<vk::Instance>(m_Instance);
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

	ThrowIfFail(vmaCreateAllocator(&allocatorInfo, &m_Allocator));
}

void
RendererVK::InitSwapchain(const VideoModeParams& p)
{
	auto surfaceCapabilities =
	  m_PhysicalDevice.getSurfaceCapabilitiesKHR(*m_Surface);
	m_SwapchainExtent = vk::Extent2D(p.width, p.height);

	vk::SwapchainCreateInfoKHR swapChainCreateInfo{};
	swapChainCreateInfo.surface = *m_Surface;
	swapChainCreateInfo.minImageCount = 2;
	swapChainCreateInfo.imageFormat = ImageFormat;
	swapChainCreateInfo.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
	swapChainCreateInfo.imageExtent = m_SwapchainExtent;
	swapChainCreateInfo.imageArrayLayers = 1;
	swapChainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
	swapChainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
	swapChainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
	swapChainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	swapChainCreateInfo.presentMode = vk::PresentModeKHR::eImmediate;
	swapChainCreateInfo.clipped = true;

	m_Swapchain = vk::raii::SwapchainKHR(m_Device, swapChainCreateInfo);
	m_SwapchainImages = m_Swapchain.getImages();
}

void
RendererVK::RecreateSwapchain(const VideoModeParams& p)
{
	m_Device.waitIdle();

	CleanupSwapchain();
	InitSwapchain(p);
	InitImageViews();
}

void
RendererVK::CleanupSwapchain()
{
	m_SwapchainImageViews.clear();
	m_Swapchain = nullptr;
}

void
RendererVK::InitImageViews()
{
	m_SwapchainImageViews.clear();
	vk::ImageViewCreateInfo createInfo{};
	createInfo.viewType = vk::ImageViewType::e2D;
	createInfo.format = ImageFormat;
	createInfo.subresourceRange = {
		vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
	};

	createInfo.components.r = vk::ComponentSwizzle::eIdentity;
	createInfo.components.g = vk::ComponentSwizzle::eIdentity;
	createInfo.components.b = vk::ComponentSwizzle::eIdentity;
	createInfo.components.a = vk::ComponentSwizzle::eIdentity;

	for (auto& image : m_SwapchainImages) {
		createInfo.image = image;
		m_SwapchainImageViews.emplace_back(m_Device, createInfo);
	}
}

void
RendererVK::InitGraphicsPipeline()
{
	auto fragmentShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/fragment.glsl"),
	  m_Device,
	  shaderc_glsl_fragment_shader);
	auto vertexShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/vertex.glsl"),
	  m_Device,
	  shaderc_glsl_vertex_shader);

	vk::PipelineShaderStageCreateInfo vertexShaderStageInfo{};
	vertexShaderStageInfo.stage = vk::ShaderStageFlagBits::eVertex;
	vertexShaderStageInfo.module = vertexShader,
	vertexShaderStageInfo.pName = "main";

	vk::PipelineShaderStageCreateInfo fragmentShaderStageInfo{};
	fragmentShaderStageInfo.stage = vk::ShaderStageFlagBits::eFragment;
	fragmentShaderStageInfo.module = fragmentShader,
	fragmentShaderStageInfo.pName = "main";

	vk::PipelineShaderStageCreateInfo shaderStages[] = {
		vertexShaderStageInfo, fragmentShaderStageInfo
	};

	std::vector dynamicStates = { vk::DynamicState::eViewport,
								  vk::DynamicState::eScissor };

	vk::PipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.dynamicStateCount =
	  static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;

	vk::PipelineViewportStateCreateInfo viewportState({}, 1, {}, 1);

	vk::PipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.depthClampEnable = vk::False;
	rasterizer.rasterizerDiscardEnable = vk::False;
	rasterizer.polygonMode = vk::PolygonMode::eFill;
	rasterizer.cullMode = vk::CullModeFlagBits::eNone;
	rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
	rasterizer.depthBiasEnable = vk::False;
	rasterizer.depthBiasSlopeFactor = 1.0f;
	rasterizer.lineWidth = 1.0f;

	vk::PipelineMultisampleStateCreateInfo multisampling{};
	multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
	multisampling.sampleShadingEnable = vk::False;

	vk::PipelineColorBlendAttachmentState colorBlendAttachment;
	colorBlendAttachment.blendEnable = vk::True;
	colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
	colorBlendAttachment.dstColorBlendFactor =
	  vk::BlendFactor::eOneMinusSrcAlpha;
	colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
	colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
	colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
	colorBlendAttachment.colorWriteMask =
	  vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	vk::PipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.logicOpEnable = vk::True;
	colorBlending.logicOp = vk::LogicOp::eCopy;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	std::vector<vk::DescriptorSetLayoutBinding> bindings = {
		vk::DescriptorSetLayoutBinding(0,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(1,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex)
	};

	vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);
	m_DescriptorSetLayout = vk::raii::DescriptorSetLayout(m_Device, layoutInfo);

	vk::PipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &*m_DescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 0;

	m_PipelineLayout = vk::raii::PipelineLayout(m_Device, pipelineLayoutInfo);

	vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo = {};
	pipelineRenderingCreateInfo.colorAttachmentCount = 1;
	pipelineRenderingCreateInfo.pColorAttachmentFormats = &ImageFormat;

	auto vertexInputInfo = GetSpriteVertexInfo();
	vk::GraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.pNext = &pipelineRenderingCreateInfo;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = m_PipelineLayout;
	pipelineInfo.renderPass = nullptr;

	m_GraphicsPipeline = vk::raii::Pipeline(m_Device, nullptr, pipelineInfo);
}

vk::PipelineVertexInputStateCreateInfo
RendererVK::GetSpriteVertexInfo()
{
	static const std::vector<vk::VertexInputBindingDescription>
	  bindingDescriptions = { vk::VertexInputBindingDescription(
		0, sizeof(RageSpriteVertex), vk::VertexInputRate::eVertex) };

	static const std::vector<vk::VertexInputAttributeDescription>
	  attributeDescriptions = {
		  { 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(RageSpriteVertex, p) },
		  { 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(RageSpriteVertex, n) },
		  { 2, 0, vk::Format::eB8G8R8A8Unorm, offsetof(RageSpriteVertex, c) },
		  { 3, 0, vk::Format::eR32G32Sfloat, offsetof(RageSpriteVertex, t) }
	  };

	vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
	vertexInputInfo.vertexBindingDescriptionCount =
	  static_cast<uint32_t>(bindingDescriptions.size());
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
	vertexInputInfo.vertexAttributeDescriptionCount =
	  static_cast<uint32_t>(attributeDescriptions.size());

	return vertexInputInfo;
}

void
RendererVK::InitCommandPool()
{
	vk::CommandPoolCreateInfo poolInfo{};
	poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	poolInfo.queueFamilyIndex = m_GraphicsQueueFamily;

	m_CommandPool = vk::raii::CommandPool(m_Device, poolInfo);
}

void
RendererVK::InitCommandBuffers()
{
	m_CommandBuffers.clear();
	vk::CommandBufferAllocateInfo allocInfo{};
	allocInfo.commandPool = m_CommandPool;
	allocInfo.level = vk::CommandBufferLevel::ePrimary;
	allocInfo.commandBufferCount = FramesInFlight;

	m_CommandBuffers = vk::raii::CommandBuffers(m_Device, allocInfo);
}

void
RendererVK::TransitionImageLayout(uint32_t imageIndex,
								  vk::ImageLayout oldLayout,
								  vk::ImageLayout newLayout,
								  vk::AccessFlags2 srcAccessMask,
								  vk::AccessFlags2 dstAccessMask,
								  vk::PipelineStageFlags2 srcStageMask,
								  vk::PipelineStageFlags2 dstStageMask)
{
	vk::ImageMemoryBarrier2 barrier{};
	barrier.srcStageMask = srcStageMask;
	barrier.srcAccessMask = srcAccessMask;
	barrier.dstStageMask = dstStageMask;
	barrier.dstAccessMask = dstAccessMask;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_SwapchainImages[imageIndex];

	vk::ImageSubresourceRange range{};
	range.aspectMask = vk::ImageAspectFlagBits::eColor;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;

	barrier.subresourceRange = range;

	vk::DependencyInfo dependencyInfo{};
	dependencyInfo.dependencyFlags = {};
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &barrier;

	m_CommandBuffers[currentFrame].pipelineBarrier2(dependencyInfo);
}

void
RendererVK::InitSyncStructures()
{
	m_PresentCompleteSemaphore.clear();
	m_RenderFinishedSemaphore.clear();
	m_InFlightFence.clear();

	for (size_t i = 0; i < FramesInFlight; i++) {
		m_PresentCompleteSemaphore.emplace_back(m_Device,
												vk::SemaphoreCreateInfo());
		m_RenderFinishedSemaphore.emplace_back(m_Device,
											   vk::SemaphoreCreateInfo());
		m_InFlightFence.emplace_back(
		  m_Device, vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
	}
}

void
RendererVK::RecordCommands(uint32_t imageIndex, uint32_t drawCount)
{
	m_CommandBuffers[currentFrame].begin({});
	TransitionImageLayout(imageIndex,
						  vk::ImageLayout::eUndefined,
						  vk::ImageLayout::eColorAttachmentOptimal,
						  {},
						  vk::AccessFlagBits2::eColorAttachmentWrite,
						  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
						  vk::PipelineStageFlagBits2::eColorAttachmentOutput);
	vk::ClearValue clearColor = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f);

	vk::RenderingAttachmentInfo attachmentInfo{};
	attachmentInfo.imageView = m_SwapchainImageViews[imageIndex];
	attachmentInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
	attachmentInfo.storeOp = vk::AttachmentStoreOp::eStore;
	attachmentInfo.clearValue = clearColor;

	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = { .offset = { 0, 0 },
								 .extent = m_SwapchainExtent };
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &attachmentInfo;

	m_CommandBuffers[currentFrame].beginRendering(renderingInfo);

	m_CommandBuffers[currentFrame].bindPipeline(
	  vk::PipelineBindPoint::eGraphics, *m_GraphicsPipeline);

	m_CommandBuffers[currentFrame].bindDescriptorSets(
	  vk::PipelineBindPoint::eGraphics,
	  *m_PipelineLayout,
	  0,
	  { *m_DescriptorSets[0] },
	  nullptr);

	vk::Buffer vertexBuffers[] = { m_SpriteVertexBuffer.get() };
	vk::DeviceSize offsets[] = { 0 };
	m_CommandBuffers[currentFrame].bindVertexBuffers(0, vertexBuffers, offsets);

	m_CommandBuffers[currentFrame].setViewport(
	  0,
	  vk::Viewport(0.0f,
				   static_cast<float>(m_SwapchainExtent.height),
				   static_cast<float>(m_SwapchainExtent.width),
				   -static_cast<float>(m_SwapchainExtent.height),
				   0.0f,
				   1.0f));
	m_CommandBuffers[currentFrame].setScissor(
	  0, vk::Rect2D(vk::Offset2D(0, 1), m_SwapchainExtent));

	if (drawCount > 0) {
		m_CommandBuffers[currentFrame].drawIndirect(
		  m_DrawCommandBuffer.get(),
		  0,
		  drawCount,
		  sizeof(Display::DrawCommand));
	}

	m_CommandBuffers[currentFrame].endRendering();

	TransitionImageLayout(imageIndex,
						  vk::ImageLayout::eColorAttachmentOptimal,
						  vk::ImageLayout::ePresentSrcKHR,
						  vk::AccessFlagBits2::eColorAttachmentWrite,
						  {},
						  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
						  vk::PipelineStageFlagBits2::eBottomOfPipe);
	m_CommandBuffers[currentFrame].end();
}

void
RendererVK::InitBatchBuffers()
{
	std::vector<vk::DescriptorPoolSize> poolSizes = { vk::DescriptorPoolSize(
	  vk::DescriptorType::eStorageBuffer, 2) };

	vk::DescriptorPoolCreateInfo poolInfo(
	  vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, poolSizes);
	m_DescriptorPool = vk::raii::DescriptorPool(m_Device, poolInfo);

	std::vector<vk::DescriptorSetLayoutBinding> bindings = {
		vk::DescriptorSetLayoutBinding(0,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(1,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex)
	};

	vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);
	m_DescriptorSetLayout = vk::raii::DescriptorSetLayout(m_Device, layoutInfo);

	vk::DescriptorSetAllocateInfo allocInfo(
	  *m_DescriptorPool, 1, &*m_DescriptorSetLayout);
	m_DescriptorSets = m_Device.allocateDescriptorSets(allocInfo);

	vk::BufferCreateInfo drawArgBufferInfo{};
	drawArgBufferInfo.size =
	  sizeof(Display::DrawCommandArgument) * MaxDrawCount;
	drawArgBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
	VmaAllocationCreateInfo drawArgAllocInfo = {};
	drawArgAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	drawArgAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	m_DrawArgumentBuffer.Init(m_Allocator, drawArgBufferInfo, drawArgAllocInfo);

	vk::BufferCreateInfo matrixBufferInfo{};
	matrixBufferInfo.size = sizeof(Display::MatrixState) * MaxDrawCount;
	matrixBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;

	VmaAllocationCreateInfo matrixAllocInfo = {};
	matrixAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	matrixAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	m_MatrixStateBuffer.Init(m_Allocator, matrixBufferInfo, matrixAllocInfo);

	vk::BufferCreateInfo vertexBufferInfo{};
	vertexBufferInfo.size = sizeof(RageSpriteVertex) * MaxDrawCount * 10;
	vertexBufferInfo.usage = vk::BufferUsageFlagBits::eVertexBuffer;

	VmaAllocationCreateInfo vertexAllocInfo = {};
	vertexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	vertexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	m_SpriteVertexBuffer.Init(m_Allocator, vertexBufferInfo, vertexAllocInfo);

	vk::BufferCreateInfo indirectBufferInfo{};
	indirectBufferInfo.size = sizeof(Display::DrawCommand) * MaxDrawCount;
	indirectBufferInfo.usage = vk::BufferUsageFlagBits::eIndirectBuffer;

	VmaAllocationCreateInfo indirectAllocInfo = {};
	indirectAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	indirectAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	m_DrawCommandBuffer.Init(
	  m_Allocator, indirectBufferInfo, indirectAllocInfo);

	vk::DescriptorBufferInfo drawArgInfo(
	  m_DrawArgumentBuffer.get(), 0, VK_WHOLE_SIZE);
	vk::DescriptorBufferInfo matrixInfo(
	  m_MatrixStateBuffer.get(), 0, VK_WHOLE_SIZE);

	std::vector<vk::WriteDescriptorSet> writes = {
		vk::WriteDescriptorSet(m_DescriptorSets[0],
							   0,
							   0,
							   1,
							   vk::DescriptorType::eStorageBuffer,
							   nullptr,
							   &drawArgInfo,
							   nullptr),
		vk::WriteDescriptorSet(m_DescriptorSets[0],
							   1,
							   0,
							   1,
							   vk::DescriptorType::eStorageBuffer,
							   nullptr,
							   &matrixInfo,
							   nullptr)
	};

	m_Device.updateDescriptorSets(writes, nullptr);
}

void
RendererVK::UpdateBatchBuffers(const Display::CommandBatcher& batcher)
{
	if (!batcher.m_IndirectCommandBuffer.empty()) {
		std::memcpy(m_DrawCommandBuffer.getMappedData(),
					batcher.m_IndirectCommandBuffer.data(),
					sizeof(Display::DrawCommand) *
					  batcher.m_IndirectCommandBuffer.size());
	}

	if (!batcher.m_IndirectCommandArgumentBuffer.empty()) {
		std::memcpy(m_DrawArgumentBuffer.getMappedData(),
					batcher.m_IndirectCommandArgumentBuffer.data(),
					sizeof(Display::DrawCommandArgument) *
					  batcher.m_IndirectCommandArgumentBuffer.size());
	}

	if (!batcher.m_SpriteVertexBuffer.empty()) {
		std::memcpy(m_SpriteVertexBuffer.getMappedData(),
					batcher.m_SpriteVertexBuffer.data(),
					sizeof(RageSpriteVertex) *
					  batcher.m_SpriteVertexBuffer.size());
	}

	if (!batcher.m_MatrixStateBuffer.empty()) {
		std::memcpy(m_MatrixStateBuffer.getMappedData(),
					batcher.m_MatrixStateBuffer.data(),
					sizeof(Display::MatrixState) *
					  batcher.m_MatrixStateBuffer.size());
	}
}
