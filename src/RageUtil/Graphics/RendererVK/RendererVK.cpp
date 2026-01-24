#ifndef NOMINMAX
#define NOMINMAX
#endif

#define VMA_IMPLEMENTATION
#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include "Core/Services/Locator.hpp"
#include <format>
#include <numbers>
#include <RageUtil/File/RageFileManager.h>
#include <RageUtil/Misc/RageMath.h>
#include <RageUtil/Graphics/RageSurface.h>

constexpr uint64_t Timeout = 1000'000'000;

RendererVK::RendererVK() {}

std::string
RendererVK::GetApiDescription() const
{
	return "Vulkan";
}

struct Swapchain
{
	enum
	{
		MaxImageCount = 3,

		NotReady = 0x12345689
	};

	uint32_t imageCount;

	vk::Format format;
	vk::Extent2D dim;

	vk::raii::SwapchainKHR swapchain = nullptr;
	vk::raii::RenderPass renderPass = nullptr;

	std::vector<vk::Image> images;
	std::array<vk::raii::ImageView, MaxImageCount> imageViews = { nullptr,
																  nullptr,
																  nullptr };
	std::array<vk::raii::Framebuffer, MaxImageCount> framebuffers = { nullptr,
																	  nullptr,
																	  nullptr };

	std::vector<vk::raii::Pipeline> pipelines;
};

struct Buffer
{
	VmaAllocation allocation;
	VkBuffer buffer;
	size_t size;
	void* mappedMemory;
};

struct Texture
{
	enum
	{
		Wrapping = 0b01,
		Filtering = 0b10,
		PossibleSamplerCount = 4,

		MaxSlots = 64
	};

	RageSurface* surface;
	VmaAllocation allocation;
	VkImage image;
	VkImageView view;
	uint32_t width;
	uint32_t height;
};

struct TriangleProgram
{
	vk::raii::ShaderModule vertexShader = nullptr;
	vk::raii::ShaderModule fragmentShader = nullptr;
	vk::raii::PipelineLayout pipelineLayout = nullptr;
	vk::raii::DescriptorSetLayout descriptorSetLayout = nullptr;
	std::array<vk::raii::DescriptorSet, Swapchain::MaxImageCount>
	  descriptorSet = { nullptr, nullptr, nullptr };
	std::array<Buffer, Swapchain::MaxImageCount> triangleBuffer;
	std::array<Buffer, Swapchain::MaxImageCount> matrixBuffer;
};

static struct VulkanState
{
	vk::PhysicalDeviceProperties properties;

	bool swapchainInvalid;
	bool borderlessWindow;

	vk::raii::Context context;
	vk::raii::Instance instance = nullptr;
	vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
	vk::raii::PhysicalDevice physicalDevice = nullptr;
	vk::raii::Device device = nullptr;
	vk::raii::CommandPool commandPool = nullptr;
	vk::raii::DescriptorPool descriptorPool = nullptr;
	uint32_t queueFamilyIndex;
	vk::raii::Queue queue = nullptr;

	std::array<vk::raii::CommandBuffer, Swapchain::MaxImageCount>
	  commandBuffer = { nullptr, nullptr, nullptr };
	std::array<vk::raii::Fence, Swapchain::MaxImageCount> fences = { nullptr,
																	 nullptr,
																	 nullptr };

	VmaAllocator allocator;

	vk::raii::SurfaceKHR surface = nullptr;
	Swapchain swapchain;

	Buffer textureStagingBuffer;
	std::unordered_map<intptr_t, Texture> textures;
	intptr_t nextTextureHandle;

	std::vector<vk::raii::Sampler> samplers;
	std::array<vk::DescriptorImageInfo, Texture::MaxSlots> descriptorImageInfos;

	struct
	{
		vk::raii::Semaphore release = nullptr;
		vk::raii::Semaphore acquire = nullptr;
	} sem;

	struct
	{
		size_t count;
		uint32_t index;
	} frame;

	TriangleProgram triangleProgram;
} g_vk = {};

static VkBool32
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
CreateSwapchain(Swapchain& swapchain, vk::raii::SwapchainKHR& oldSwapchain)
{
	vk::SurfaceFormatKHR format = {};
	vk::SurfaceCapabilitiesKHR surfaceCapabilities =
	  g_vk.physicalDevice.getSurfaceCapabilitiesKHR(g_vk.surface);

	if (surfaceCapabilities.minImageCount > Swapchain::MaxImageCount) {
		throw std::runtime_error(":(");
	}

	VkBool32 queueSupportsSurface = g_vk.physicalDevice.getSurfaceSupportKHR(
	  g_vk.queueFamilyIndex, g_vk.surface);

	if (queueSupportsSurface == false) {
		throw std::runtime_error(":(");
	}

	swapchain.dim = surfaceCapabilities.currentExtent;
	const uint32_t& width = swapchain.dim.width;
	const uint32_t& height = swapchain.dim.height;

	if (width == 0 && height == 0) {
		throw std::runtime_error(":(");
	}

	{
		std::vector<vk::SurfaceFormatKHR> formats =
		  g_vk.physicalDevice.getSurfaceFormatsKHR(g_vk.surface);
		for (size_t i = 0; i < formats.size(); i++) {
			bool spaceOk =
			  formats[i].colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
			bool formatOk = (formats[i].format == vk::Format::eR8G8B8A8Unorm) ||
							(formats[i].format == vk::Format::eB8G8R8A8Unorm);
			if (spaceOk && formatOk) {
				format = formats[i];
				break;
			}
		}

		if (format.format == vk::Format::eUndefined) {
			throw std::runtime_error(":(");
		}

		swapchain.format = format.format;
	}

	// Create swap chain
	{
		vk::CompositeAlphaFlagBitsKHR compositeAlpha =
		  vk::CompositeAlphaFlagBitsKHR::eInherit;
		const auto scaFlags = surfaceCapabilities.supportedCompositeAlpha;
		if (scaFlags & vk::CompositeAlphaFlagBitsKHR::eOpaque) {
			compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
		} else if (scaFlags & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied) {
			compositeAlpha = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
		}

		vk::SurfaceFullScreenExclusiveInfoEXT exclusiveFullScreenInfo = {

		};
		exclusiveFullScreenInfo.fullScreenExclusive =
		  g_vk.borderlessWindow ? vk::FullScreenExclusiveEXT::eDisallowed
								: vk::FullScreenExclusiveEXT::eAllowed;

		vk::SwapchainCreateInfoKHR swapchainInfo = {

		};
		swapchainInfo.pNext = &exclusiveFullScreenInfo;
		swapchainInfo.surface = g_vk.surface;
		swapchainInfo.minImageCount =
		  std::clamp(uint32_t(Swapchain::MaxImageCount),
					 surfaceCapabilities.minImageCount,
					 surfaceCapabilities.maxImageCount);
		swapchainInfo.imageFormat = format.format;
		swapchainInfo.imageColorSpace = format.colorSpace;
		swapchainInfo.imageExtent = swapchain.dim;
		swapchainInfo.imageArrayLayers = 1;
		swapchainInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
		// TODO: If graphics and present queues are different, this must be
		// VK_SHARING_MODE_CONCURRENT
		swapchainInfo.imageSharingMode = vk::SharingMode::eExclusive;
		swapchainInfo.queueFamilyIndexCount = 1;
		swapchainInfo.pQueueFamilyIndices = &g_vk.queueFamilyIndex;
		swapchainInfo.preTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
		swapchainInfo.compositeAlpha = compositeAlpha;
		// TODO: Set up VK_PRESENT_MODE_MAILBOX_KHR (preferred) and
		// VK_PRESENT_MODE_FIFO_KHR (fallback) for vsync
		swapchainInfo.presentMode = vk::PresentModeKHR::eImmediate;
		swapchainInfo.clipped = vk::True;
		swapchainInfo.oldSwapchain = oldSwapchain;

		swapchain.swapchain =
		  vk::raii::SwapchainKHR(g_vk.device, swapchainInfo);
	}

	// Create swapchain images, image views
	{
		swapchain.images = swapchain.swapchain.getImages();
		swapchain.imageCount = swapchain.images.size();
		assert(swapchain.imageCount <= Swapchain::MaxImageCount);

		for (size_t i = 0; i < swapchain.imageCount; i++) {
			vk::ImageViewCreateInfo imageViewInfo = {};
			imageViewInfo.image = swapchain.images[i];
			imageViewInfo.viewType = vk::ImageViewType::e2D;
			imageViewInfo.format = format.format;
			imageViewInfo.subresourceRange.aspectMask =
			  vk::ImageAspectFlagBits::eColor;
			imageViewInfo.subresourceRange.levelCount = 1;
			imageViewInfo.subresourceRange.layerCount = 1;

			swapchain.imageViews[i] =
			  vk::raii::ImageView(g_vk.device, imageViewInfo);
		}
	}

	// Create render pass
	{
		vk::AttachmentDescription attachment = {};
		attachment.format = format.format;
		attachment.samples = vk::SampleCountFlagBits::e1;
		attachment.loadOp = vk::AttachmentLoadOp::eClear;
		attachment.storeOp = vk::AttachmentStoreOp::eStore;
		attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
		attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
		attachment.initialLayout = vk::ImageLayout::eUndefined;
		attachment.finalLayout = vk::ImageLayout::ePresentSrcKHR;

		vk::AttachmentReference attachmentReference = {};
		attachmentReference.attachment = 0;
		attachmentReference.layout = vk::ImageLayout::eColorAttachmentOptimal;

		vk::SubpassDescription subpass = {};
		subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &attachmentReference;

		vk::SubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask =
		  vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dependency.dstStageMask =
		  vk::PipelineStageFlagBits::eColorAttachmentOutput;
		dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		dependency.dependencyFlags = vk::DependencyFlagBits::eByRegion;

		vk::RenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.attachmentCount = 1;
		renderPassInfo.pAttachments = &attachment;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		swapchain.renderPass = g_vk.device.createRenderPass(renderPassInfo);
	}

	// Create framebuffers
	{
		for (size_t i = 0; i < swapchain.imageCount; i++) {
			vk::FramebufferCreateInfo framebufferInfo = {};
			framebufferInfo.renderPass = swapchain.renderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = &*swapchain.imageViews[i];
			framebufferInfo.width = width;
			framebufferInfo.height = height;
			framebufferInfo.layers = 1;

			swapchain.framebuffers[i] =
			  g_vk.device.createFramebuffer(framebufferInfo);
		}
	}

	// Create graphics pipeline for quads
	{
		vk::GraphicsPipelineCreateInfo pipelineInfo = {};

		vk::PipelineShaderStageCreateInfo shaderInfo[2] = {};
		auto& vertStageInfo = shaderInfo[0];
		auto& fragStageInfo = shaderInfo[1];

		vertStageInfo.stage = vk::ShaderStageFlagBits::eVertex;
		vertStageInfo.module = *g_vk.triangleProgram.vertexShader;
		vertStageInfo.pName = "main";

		fragStageInfo.stage = vk::ShaderStageFlagBits::eFragment;
		fragStageInfo.module = *g_vk.triangleProgram.fragmentShader;
		fragStageInfo.pName = "main";

		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderInfo;

		vk::PipelineVertexInputStateCreateInfo vertexInputInfo = {};
		pipelineInfo.pVertexInputState = &vertexInputInfo;

		vk::PipelineInputAssemblyStateCreateInfo inputAssemblyInfo = {};
		inputAssemblyInfo.topology = vk::PrimitiveTopology::eTriangleList;
		pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;

		// Upside down viewport to match the rest of the game
		vk::Viewport viewport = {};
		viewport.x = 0;
		viewport.y = float(height);
		viewport.width = float(width);
		viewport.height = -float(height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vk::Rect2D scissor = { { 0, 1 }, { width, height } };

		vk::PipelineViewportStateCreateInfo viewportInfo = {};
		viewportInfo.viewportCount = 1;
		viewportInfo.pViewports = &viewport;
		viewportInfo.scissorCount = 1;
		viewportInfo.pScissors = &scissor;
		pipelineInfo.pViewportState = &viewportInfo;

		vk::PipelineRasterizationStateCreateInfo rasterizationInfo = {};
		rasterizationInfo.lineWidth = 1.0f;
		rasterizationInfo.cullMode = vk::CullModeFlagBits::eBack;
		rasterizationInfo.frontFace = vk::FrontFace::eCounterClockwise;
		pipelineInfo.pRasterizationState = &rasterizationInfo;

		vk::PipelineMultisampleStateCreateInfo multisampleInfo = {};
		multisampleInfo.rasterizationSamples = vk::SampleCountFlagBits::e1;
		pipelineInfo.pMultisampleState = &multisampleInfo;

		vk::PipelineColorBlendAttachmentState colorBlendAttachment = {};
		// Equivalent to sm's BLEND_NORMAL. Changing these doesn't require a new
		// pipeline
		colorBlendAttachment.blendEnable = VK_TRUE;
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

		vk::PipelineColorBlendStateCreateInfo colorBlendInfo = {};
		colorBlendInfo.attachmentCount = 1;
		colorBlendInfo.pAttachments = &colorBlendAttachment;
		pipelineInfo.pColorBlendState = &colorBlendInfo;

		vk::PipelineDynamicStateCreateInfo dynamicInfo = {};
		pipelineInfo.pDynamicState = &dynamicInfo;

		pipelineInfo.layout = g_vk.triangleProgram.pipelineLayout;
		pipelineInfo.renderPass = swapchain.renderPass;

		swapchain.pipelines.emplace_back(
		  g_vk.device.createGraphicsPipeline(nullptr, pipelineInfo));
	}
}

void
RecreateSwapchain(Swapchain& swapchain)
{
	g_vk.device.waitIdle();

	// Create a new swapchain first so the driver can reuse resources, then
	// destroy the old swapchain.
	Swapchain newSwapchain{};
	CreateSwapchain(newSwapchain, swapchain.swapchain);
	g_vk.swapchain = std::move(newSwapchain);
}

void
CreatePersistentlyMappedBuffer(Buffer& buffer,
							   size_t size,
							   VkBufferUsageFlags bufferUsage)
{
	VmaAllocationCreateInfo allocCreateInfo = {};
	allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	allocCreateInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	allocCreateInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = bufferUsage;

	VmaAllocationInfo allocInfo = {};
	ThrowIfFail(vmaCreateBuffer(g_vk.allocator,
								&bufferInfo,
								&allocCreateInfo,
								&buffer.buffer,
								&buffer.allocation,
								&allocInfo));
	buffer.size = size;
	buffer.mappedMemory = allocInfo.pMappedData;
}

void
DestroyTexture(Texture& texture)
{
	if (texture.image) {
		vmaDestroyImage(g_vk.allocator, texture.image, texture.allocation);
	}
	if (texture.view) {
		vkDestroyImageView(*g_vk.device, texture.view, 0);
	}
	texture = {};
}

void
CreateTexture(Texture& texture, int32_t width, int32_t height)
{
	uint32_t w = uint32_t(power_of_two(width));
	uint32_t h = uint32_t(power_of_two(height));

	{
		VmaAllocationCreateInfo allocCreateInfo = {};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.extent = { w, h, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.usage =
		  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		VmaAllocationInfo allocInfo = {};
		ThrowIfFail(vmaCreateImage(g_vk.allocator,
								   &imageInfo,
								   &allocCreateInfo,
								   &texture.image,
								   &texture.allocation,
								   &allocInfo));
		assert(texture.image);
	}

	{
		VkImageViewCreateInfo viewInfo = {
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
		};
		viewInfo.image = texture.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		ThrowIfFail(
		  vkCreateImageView(*g_vk.device, &viewInfo, 0, &texture.view));
		assert(texture.view);
	}

	texture.width = w;
	texture.height = h;
}

void
CreateTriangleProgram(TriangleProgram& triangleProgram)
{
	triangleProgram.fragmentShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/fragment.glsl"),
	  g_vk.device,
	  shaderc_glsl_fragment_shader);
	triangleProgram.vertexShader = LoadShaderFromFile(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/vertex.glsl"),
	  g_vk.device,
	  shaderc_glsl_vertex_shader);

	// Pipeline layout
	std::vector<vk::DescriptorSetLayoutBinding> bindings = {
		vk::DescriptorSetLayoutBinding(0,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(1,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(
		  2,
		  vk::DescriptorType::eCombinedImageSampler,
		  Texture::MaxSlots,
		  vk::ShaderStageFlagBits::eFragment)
	};

	vk::DescriptorSetLayoutCreateInfo dsLayoutInfo({}, bindings);
	triangleProgram.descriptorSetLayout =
	  vk::raii::DescriptorSetLayout(g_vk.device, dsLayoutInfo);

	vk::PipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.pSetLayouts = &*triangleProgram.descriptorSetLayout;
	layoutInfo.setLayoutCount = 1;
	triangleProgram.pipelineLayout =
	  g_vk.device.createPipelineLayout(layoutInfo);

	for (size_t i = 0; i < Swapchain::MaxImageCount; i++) {
		vk::DescriptorSetAllocateInfo setAllocateInfo = {};
		setAllocateInfo.descriptorPool = g_vk.descriptorPool;
		setAllocateInfo.descriptorSetCount = 1;
		setAllocateInfo.pSetLayouts = &*triangleProgram.descriptorSetLayout;

		triangleProgram.descriptorSet[i] =
		  std::move(g_vk.device.allocateDescriptorSets(setAllocateInfo)[0]);

		// Buffers
		CreatePersistentlyMappedBuffer(triangleProgram.triangleBuffer[i],
									   64 * 1024 * 1024,
									   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
		CreatePersistentlyMappedBuffer(triangleProgram.matrixBuffer[i],
									   2 * UINT16_MAX,
									   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
		assert(triangleProgram.matrixBuffer[i].buffer);

		vk::DescriptorBufferInfo bufferInfo[2] = {};
		bufferInfo[0].buffer = g_vk.triangleProgram.triangleBuffer[i].buffer;
		bufferInfo[0].range = VK_WHOLE_SIZE;
		bufferInfo[1].buffer = g_vk.triangleProgram.matrixBuffer[i].buffer;
		bufferInfo[1].range = VK_WHOLE_SIZE;

		vk::WriteDescriptorSet writeDescriptorSets[2] = {};
		writeDescriptorSets[0].dstSet = g_vk.triangleProgram.descriptorSet[i];
		writeDescriptorSets[0].dstBinding = 0;
		writeDescriptorSets[0].descriptorCount = 1;
		writeDescriptorSets[0].descriptorType =
		  vk::DescriptorType::eStorageBuffer;
		writeDescriptorSets[0].pBufferInfo = &bufferInfo[0];
		writeDescriptorSets[1].dstSet = g_vk.triangleProgram.descriptorSet[i];
		writeDescriptorSets[1].dstBinding = 1;
		writeDescriptorSets[1].descriptorCount = 1;
		writeDescriptorSets[1].descriptorType =
		  vk::DescriptorType::eStorageBuffer;
		writeDescriptorSets[1].pBufferInfo = &bufferInfo[1];

		g_vk.device.updateDescriptorSets(writeDescriptorSets, {});
	}
}

void
RendererVK::InitializeRenderer(const VideoModeParams& p)
{
	vkb::InstanceBuilder builder;
	auto instanceResult =
	  builder
#ifdef _DEBUG
		.request_validation_layers(true)
		.use_default_debug_messenger()
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT)
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT)
		.add_validation_feature_enable(
		  VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
		.set_debug_callback(VulkanDebugCallback)
#endif
		.require_api_version(1, 3, 0)
#ifdef _WIN32
		.enable_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)
#endif
		.build();
	if (!instanceResult) {
		Fail();
	}

	g_vk.instance = vk::raii::Instance(g_vk.context, instanceResult->instance);
	g_vk.debugMessenger = vk::raii::DebugUtilsMessengerEXT(
	  g_vk.instance, instanceResult->debug_messenger);

#ifdef _WIN32
	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hwnd = GraphicsWindow::GetHwnd();
	createInfo.hinstance = GetModuleHandle(nullptr);
	g_vk.surface = g_vk.instance.createWin32SurfaceKHR(createInfo);
#else
#error todo
#endif

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
	vkFeatures.samplerAnisotropy = vk::True;
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
		.set_surface(static_cast<vk::SurfaceKHR>(g_vk.surface))
		.select();
	if (!physicalDeviceResult) {
		Fail();
	}

	vkb::DeviceBuilder deviceBuilder(*physicalDeviceResult);
	auto deviceResult = deviceBuilder.build();
	if (!deviceResult) {
		Fail();
	}

	g_vk.physicalDevice = vk::raii::PhysicalDevice(
	  g_vk.instance, physicalDeviceResult->physical_device);
	g_vk.device = vk::raii::Device(g_vk.physicalDevice, deviceResult->device);
	g_vk.queue = vk::raii::Queue(
	  g_vk.device, deviceResult->get_queue(vkb::QueueType::graphics).value());

	// Initialize VulkanMemoryAllocator
	{
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorInfo.physicalDevice = *g_vk.physicalDevice;
		allocatorInfo.device = *g_vk.device;
		allocatorInfo.instance = *g_vk.instance;

		ThrowIfFail(vmaCreateAllocator(&allocatorInfo, &g_vk.allocator));
	}

	// Create semaphores
	{
		vk::SemaphoreCreateInfo semaphoreInfo = {};
		g_vk.sem.release = g_vk.device.createSemaphore(semaphoreInfo);
		g_vk.sem.acquire = g_vk.device.createSemaphore(semaphoreInfo);
	}

	// Create command pool, buffer
	{
		vk::CommandPoolCreateInfo commandPoolInfo = {};
		commandPoolInfo.flags =
		  vk::CommandPoolCreateFlagBits::eTransient |
		  vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		commandPoolInfo.queueFamilyIndex = g_vk.queueFamilyIndex;

		g_vk.commandPool = g_vk.device.createCommandPool(commandPoolInfo);

		vk::CommandBufferAllocateInfo allocateInfo = {};
		allocateInfo.commandPool = g_vk.commandPool;
		allocateInfo.level = vk::CommandBufferLevel::ePrimary;
		// We need one for each swapchain image, but don't have a swapchain yet
		allocateInfo.commandBufferCount = Swapchain::MaxImageCount;
		g_vk.commandBuffer[0] =
		  std::move(g_vk.device.allocateCommandBuffers(allocateInfo)[0]);
	}

	// Create fences
	{
		vk::FenceCreateInfo fenceInfo = {};
		fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
		for (size_t i = 0; i < Swapchain::MaxImageCount; i++) {
			g_vk.fences[i] = g_vk.device.createFence(fenceInfo);
		}
	}

	// Create descriptor pool (based on TriangleProgram)
	{
		vk::DescriptorPoolSize poolSizes[2] = {};
		poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
		poolSizes[0].descriptorCount = 2;
		poolSizes[1].type = vk::DescriptorType::eCombinedImageSampler;
		poolSizes[1].descriptorCount = Texture::MaxSlots;

		vk::DescriptorPoolCreateInfo descriptorPoolInfo = {};
		descriptorPoolInfo.poolSizeCount = 1;
		descriptorPoolInfo.pPoolSizes = poolSizes;
		descriptorPoolInfo.maxSets = Swapchain::MaxImageCount;

		g_vk.descriptorPool =
		  g_vk.device.createDescriptorPool(descriptorPoolInfo);
	}

	// Create staging buffer
	{
		uint32_t dim = GetMaxTextureSize();
		CreatePersistentlyMappedBuffer(g_vk.textureStagingBuffer,
									   dim * dim * sizeof(uint32_t),
									   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	}

	// Create samplers. Just create all combinations up front as SM only
	// supports wrapping and filtering toggles.
	{
		vk::SamplerCreateInfo samplerInfo = {};

		for (size_t i = 0; i < Texture::PossibleSamplerCount; i++) {
			samplerInfo.magFilter = (i & Texture::Filtering)
									  ? vk::Filter::eLinear
									  : vk::Filter::eNearest;
			samplerInfo.minFilter = (i & Texture::Filtering)
									  ? vk::Filter::eLinear
									  : vk::Filter::eNearest;
			samplerInfo.addressModeU =
			  (i & Texture::Wrapping) ? vk::SamplerAddressMode::eRepeat
									  : vk::SamplerAddressMode::eClampToBorder;
			samplerInfo.addressModeV =
			  (i & Texture::Wrapping) ? vk::SamplerAddressMode::eRepeat
									  : vk::SamplerAddressMode::eClampToBorder;
			g_vk.samplers.emplace_back(g_vk.device.createSampler(samplerInfo));
		}
	}

	CreateTriangleProgram(g_vk.triangleProgram);

	// Create empty texture
	{
		RageSurface* img = CreateSurface(
		  1, 1, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
		CreateTexture(img);
	}

	auto oldSwapchain = std::move(g_vk.swapchain);
	g_vk.swapchain = Swapchain{};
	g_vk.borderlessWindow = p.bWindowIsFullscreenBorderless;

	CreateSwapchain(g_vk.swapchain, oldSwapchain.swapchain);
}

bool
RendererVK::IsD3DInternal()
{
	return false;
}

intptr_t
RendererVK::CreateTexture(RageSurface* img)
{
	Texture tex = {};
	tex.surface = img;
	::CreateTexture(tex, img->w, img->h);

	ptrdiff_t handle = g_vk.nextTextureHandle++;
	g_vk.textures.insert({ handle, tex });
	UpdateTexture(handle, img, 0, 0, img->w, img->h);
	return handle;
}

void
RendererVK::UpdateTexture(intptr_t texHandle,
						  RageSurface* img,
						  int xoffset,
						  int yoffset,
						  int width,
						  int height)
{
	assert(xoffset == 0);
	assert(yoffset == 0);
	assert(img->w == width);
	assert(img->pitch == width * sizeof(uint32_t));
	assert(img->h == height);

	vk::CommandBufferAllocateInfo cmdBufferInfo = {};
	cmdBufferInfo.level = vk::CommandBufferLevel::ePrimary;
	cmdBufferInfo.commandPool = g_vk.commandPool;
	cmdBufferInfo.commandBufferCount = 1;

	vk::raii::CommandBuffer copyCommand =
	  std::move(g_vk.device.allocateCommandBuffers(cmdBufferInfo)[0]);

	vk::CommandBufferBeginInfo beginInfo = {};
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	copyCommand.begin(beginInfo);

	// TODO: Crashes if texHandle is invalid. Log instead
	Texture& tex = g_vk.textures.at(texHandle);

	memcpy(g_vk.textureStagingBuffer.mappedMemory,
		   img->pixels,
		   img->w * img->h * sizeof(uint32_t));

	// At this point, the texture is on the device (memcpy above), but in the
	// wrong place and wrong layout (literally how pixels are arranged in
	// memory). Getting it from the textureStagingBuffer VkBuffer to the
	// tex.image VkImage is a 3 step process:

	// VkImage old layout -> transfer layout
	vk::ImageMemoryBarrier barrier = {};
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.oldLayout = vk::ImageLayout::eUndefined;
	barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = tex.image;
	barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	copyCommand.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
								vk::PipelineStageFlagBits::eTransfer,
								{},
								{},
								{},
								{ barrier });

	// Copy VkBuffer -> VkImage
	vk::BufferImageCopy imageCopy = {};
	imageCopy.imageExtent = VkExtent3D{ tex.width, tex.height, 1 };
	imageCopy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	imageCopy.imageSubresource.mipLevel = 0;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;
	// TODO: Expect this to fail when textures are reuploaded on demand
	copyCommand.copyBufferToImage(g_vk.textureStagingBuffer.buffer,
								  tex.image,
								  vk::ImageLayout::eTransferDstOptimal,
								  { imageCopy });

	// VkImage transfer layout -> shader readonly layout
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	copyCommand.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
								vk::PipelineStageFlagBits::eFragmentShader,
								{},
								{},
								{},
								{ barrier });

	copyCommand.end();

	vk::SubmitInfo submitInfo = {};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &*copyCommand;

	g_vk.queue.submit({ submitInfo });

	// We just use the one staging buffer, so we can't reuse that memory until
	// transfer is complete. We also can't free the command buffer until then.
	// This is probably unecessary serialization
	g_vk.queue.waitIdle();
}

void
RendererVK::DeleteTexture(intptr_t texHandle)
{
	if (!g_vk.textures.count(texHandle))
		return;

	Texture& tex = g_vk.textures.at(texHandle);

	g_vk.queue.waitIdle();
	DestroyTexture(tex);
	g_vk.textures.erase(texHandle);
}

void
RendererVK::ResolutionChanged()
{
	g_vk.swapchainInvalid = true;
}

void
RendererVK::OnRender(const ActualVideoModeParams* p,
					 Display::CommandBatcher& batcher)
{
	if (g_vk.swapchainInvalid) {
		RecreateSwapchain(g_vk.swapchain);
		g_vk.swapchainInvalid = false;
	}

	g_vk.frame.count++;
	auto [result, imageIndex] =
	  g_vk.swapchain.swapchain.acquireNextImage(Timeout, *g_vk.sem.acquire, {});
	ThrowIfFail(result);

	ThrowIfFail(g_vk.device.waitForFences(
	  { g_vk.fences[g_vk.frame.index] }, VK_FALSE, Timeout));

	g_vk.device.resetFences({ g_vk.fences[g_vk.frame.index] });

	vk::raii::CommandBuffer& commandBuffer =
	  g_vk.commandBuffer[g_vk.frame.index];

	{
		vk::CommandBufferBeginInfo cmdBeginInfo = {};
		cmdBeginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		commandBuffer.begin(cmdBeginInfo);
	}

	{
		vk::ClearColorValue color = { 0.0f, 0.0f, 0.0f, 1.0f };
		vk::ClearValue clearValue = { color };

		vk::RenderPassBeginInfo passBeginInfo = {};
		passBeginInfo.renderPass = g_vk.swapchain.renderPass;
		passBeginInfo.framebuffer =
		  g_vk.swapchain.framebuffers[g_vk.frame.index];
		passBeginInfo.renderArea.extent = g_vk.swapchain.dim;
		passBeginInfo.clearValueCount = 1;
		passBeginInfo.pClearValues = &clearValue;
		commandBuffer.beginRenderPass(passBeginInfo,
									  vk::SubpassContents::eInline);
	}

	if (batcher.m_TriangleBuffer.size() > 0) {
		VkImageView nullView = g_vk.textures.at(0).view;
		for (size_t i = 0; i < Texture::MaxSlots; i++) {
			g_vk.descriptorImageInfos[i].sampler = g_vk.samplers[0];
			g_vk.descriptorImageInfos[i].imageView = nullView;
			g_vk.descriptorImageInfos[i].imageLayout =
			  vk::ImageLayout::eShaderReadOnlyOptimal;
		}

		std::map<std::pair<uint8_t, intptr_t>, int> textureLocation;
		for (auto& triangle : batcher.m_TriangleBuffer) {
			const auto& renderState =
			  batcher.m_RenderStateBuffer[triangle.TextureIndex];

			if (!renderState.textureHandle) {
				continue;
			}

			const uint8_t textureSettings =
			  ((uint8_t)renderState.textureWrapping
			   << (Texture::Wrapping - 1)) |
			  ((uint8_t)renderState.textureFiltering
			   << (Texture::Filtering - 1));

			auto it = textureLocation.find(
			  { textureSettings, renderState.textureHandle });
			if (it != textureLocation.end()) {
				triangle.TextureIndex = it->second;
			} else {
				assert(textureLocation.size() < Texture::MaxSlots);
				g_vk.descriptorImageInfos[textureLocation.size()].sampler =
				  g_vk.samplers[textureSettings];
				g_vk.descriptorImageInfos[textureLocation.size()].imageView =
				  g_vk.textures[renderState.textureHandle].view;

				triangle.TextureIndex = textureLocation.size() + 1;
				textureLocation.emplace_hint(
				  it,
				  std::make_pair(
					std::make_pair(textureSettings, renderState.textureHandle),
					triangle.TextureIndex));
			}
		}

		vk::WriteDescriptorSet writeDescriptor = {};
		writeDescriptor.dstSet =
		  g_vk.triangleProgram.descriptorSet[g_vk.frame.index];
		writeDescriptor.dstBinding = 2;
		writeDescriptor.descriptorCount = Texture::MaxSlots;
		writeDescriptor.descriptorType =
		  vk::DescriptorType::eCombinedImageSampler;
		writeDescriptor.pImageInfo = g_vk.descriptorImageInfos.data();
		g_vk.device.updateDescriptorSets({ writeDescriptor }, {});

		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
								   g_vk.swapchain.pipelines[0]);

		size_t idx = g_vk.frame.index;

		memcpy(g_vk.triangleProgram.triangleBuffer[idx].mappedMemory,
			   batcher.m_TriangleBuffer.data(),
			   batcher.m_TriangleBuffer.size() * sizeof(Display::Triangle));

		memcpy(g_vk.triangleProgram.matrixBuffer[idx].mappedMemory,
			   batcher.m_MatrixStateBuffer.data(),
			   batcher.m_MatrixStateBuffer.size() *
				 sizeof(Display::MatrixState));

		commandBuffer.bindDescriptorSets(
		  vk::PipelineBindPoint::eGraphics,
		  g_vk.triangleProgram.pipelineLayout,
		  0,
		  { g_vk.triangleProgram.descriptorSet[idx] },
		  {});

		commandBuffer.draw(
		  uint32_t(batcher.m_TriangleBuffer.size() * 3), 1, 0, 0);
	}
	commandBuffer.endRenderPass();
	commandBuffer.end();
	{
		vk::PipelineStageFlags submitStageMask =
		  vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submitInfo = {};
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &*g_vk.sem.acquire;
		submitInfo.pWaitDstStageMask = &submitStageMask;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &*commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &*g_vk.sem.release;
		g_vk.queue.submit({ submitInfo }, g_vk.fences[g_vk.frame.index]);

		vk::PresentInfoKHR presentInfo = {};
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &*g_vk.sem.release;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &*g_vk.swapchain.swapchain;
		presentInfo.pImageIndices = &g_vk.frame.index;

		auto result = g_vk.queue.presentKHR(presentInfo);

		// The game notifies RageDisplay about resolution changes but not
		// alt-tabs, both of which invaldiate the swapchain.
		g_vk.swapchainInvalid = result == vk::Result::eErrorOutOfDateKHR ||
								result == vk::Result::eSuboptimalKHR;
		if (!g_vk.swapchainInvalid && result != vk::Result::eSuccess) {
			ThrowIfFail(result);
		}
	}
}

void
RendererVK::ClearAllTextures()
{
	assert(false && "Never called?");
}

RendererVK::~RendererVK()
{
	g_vk.device.waitIdle();
}

int
RendererVK::GetMaxTextureSize()
{
	return std::min(
	  4096u, g_vk.physicalDevice.getProperties().limits.maxImageDimension2D);
}
