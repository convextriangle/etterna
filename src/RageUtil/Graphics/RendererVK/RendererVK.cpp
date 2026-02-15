#ifndef NOMINMAX // >:3
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

constexpr uint64_t Timeout = 1000'000'000;

RendererVK::RendererVK()
  : m_Samplers{ nullptr, nullptr, nullptr, nullptr }
{
}

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
	InitTextureSamplers();
}

/// ----------------------------------------
/// here be hazards and unsignaled fences...
/// ----------------------------------------
void
RendererVK::OnRender(const ActualVideoModeParams* p,
					 Display::CommandBatcher& batcher)
{
	ThrowIfFail(m_Device.waitForFences(
	  *m_InFlightFence[m_CurrentFrame], vk::True, Timeout));

	UpdateBatchBuffers(batcher);

	auto [result, imageIndex] = m_Swapchain.acquireNextImage(
	  Timeout, *m_PresentCompleteSemaphore[m_CurrentFrame], nullptr);

	if (result == vk::Result::eErrorOutOfDateKHR || m_SwapchainIsInvalid) {
		RecreateSwapchain(*p);
		m_SwapchainIsInvalid = false;
		return;
	}
	ThrowIfFail(result);

	m_Device.resetFences(*m_InFlightFence[m_CurrentFrame]);
	m_CommandBuffers[m_CurrentFrame].reset();
	RecordCommands(imageIndex, batcher.m_IndexBuffer.size());

	vk::PipelineStageFlags waitDestinationStageMask(
	  vk::PipelineStageFlagBits::eColorAttachmentOutput);

	vk::SubmitInfo submitInfo{};
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &*m_PresentCompleteSemaphore[m_CurrentFrame];
	submitInfo.pWaitDstStageMask = &waitDestinationStageMask;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &*m_CommandBuffers[m_CurrentFrame];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &*m_RenderFinishedSemaphore[imageIndex];
	m_GraphicsQueue.submit(submitInfo, *m_InFlightFence[m_CurrentFrame]);

	vk::PresentInfoKHR presentInfoKHR{};
	presentInfoKHR.waitSemaphoreCount = 1;
	presentInfoKHR.pWaitSemaphores = &*m_RenderFinishedSemaphore[imageIndex];
	presentInfoKHR.swapchainCount = 1;
	presentInfoKHR.pSwapchains = &*m_Swapchain;
	presentInfoKHR.pImageIndices = &imageIndex;

	try {
		result = m_PresentQueue.presentKHR(presentInfoKHR);
	} catch (vk::OutOfDateKHRError error) {
		RecreateSwapchain(*p);
		return;
	}

	if (result == vk::Result::eSuboptimalKHR) {
		RecreateSwapchain(*p);
		return;
	}

	m_CurrentFrame = (m_CurrentFrame + 1) % FramesInFlight;
}

bool
RendererVK::IsD3DInternal()
{
	return false;
}

intptr_t
RendererVK::CreateTexture(RageSurface* img)
{
	intptr_t currentHandle = m_TextureCounter++;

	Texture texture = {};
	texture.width = power_of_two(img->w);
	texture.height = power_of_two(img->h);

	VmaAllocationCreateInfo allocCreateInfo = {};
	allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent = { texture.width, texture.height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.usage =
	  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	VmaAllocationInfo allocInfo = {};
	ThrowIfFail(vmaCreateImage(m_Allocator,
							   &imageInfo,
							   &allocCreateInfo,
							   &texture.image,
							   &texture.allocation,
							   &allocInfo));

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = texture.image;
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = vk::Format::eR8G8B8A8Unorm;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	texture.view = (*m_Device).createImageView(viewInfo);

	m_Textures.insert({ currentHandle, texture });

	UpdateTexture(currentHandle, img, 0, 0, img->w, img->h);
	return currentHandle;
}

void
RendererVK::UpdateTexture(intptr_t textureHandle,
						  RageSurface* img,
						  int xOffset,
						  int yOffset,
						  int width,
						  int height)
{
	assert(xOffset == 0);
	assert(yOffset == 0);
	assert(width == img->w);
	assert(height == img->h);
	assert(img->pitch == width * sizeof(uint32_t));
	assert(m_Textures.contains(textureHandle));

	vk::CommandBufferAllocateInfo bufferInfo = {};
	bufferInfo.level = vk::CommandBufferLevel::ePrimary;
	bufferInfo.commandPool = m_CommandPool;
	bufferInfo.commandBufferCount = 1;

	auto buffers = m_Device.allocateCommandBuffers(bufferInfo);
	assert(buffers.size() == 1);
	auto& copyBuffer = buffers[0];

	vk::CommandBufferBeginInfo beginInfo = {};
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	copyBuffer.begin(beginInfo);

	auto& texture = m_Textures[textureHandle];
	std::memcpy(m_TextureBuffer.GetMappedData(),
				img->pixels,
				static_cast<size_t>(img->h) * img->w * sizeof(uint32_t));

	vk::ImageMemoryBarrier barrier = {};
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.oldLayout = vk::ImageLayout::eUndefined;
	barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture.image;
	barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });

	vk::BufferImageCopy imageCopy = {};
	imageCopy.imageExtent = vk::Extent3D{ texture.width, texture.height, 1 };
	imageCopy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	imageCopy.imageSubresource.mipLevel = 0;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;
	copyBuffer.copyBufferToImage(m_TextureBuffer.buffer,
								 texture.image,
								 vk::ImageLayout::eTransferDstOptimal,
								 { imageCopy });

	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
							   vk::PipelineStageFlagBits::eFragmentShader,
							   {},
							   {},
							   {},
							   { barrier });

	copyBuffer.end();

	vk::SubmitInfo submitInfo = {};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &(*copyBuffer);
	m_GraphicsQueue.submit({ submitInfo });
	m_GraphicsQueue.waitIdle();
}

void
RendererVK::DeleteTexture(intptr_t handle)
{
	assert(handle != 0);
	m_GraphicsQueue.waitIdle();

	DestroyTexture(m_Textures[handle]);
	m_Textures.erase(handle);
}

void
RendererVK::ClearAllTextures()
{
	m_GraphicsQueue.waitIdle();
	auto emptyTexture = m_Textures[0];
	m_Textures.clear();
	m_Textures[0] = emptyTexture;
}

RageSurface*
RendererVK::CreateScreenshot()
{
	auto props =
	  m_PhysicalDevice.getFormatProperties(vk::Format::eR8G8B8A8Unorm);

	bool supportsBlitting =
	  (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc) &&
	  (props.linearTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);

	auto sourceImage =
	  m_SwapchainImages[((int)m_CurrentFrame - 1 + FramesInFlight) %
						FramesInFlight];

	vk::ImageCreateInfo destImageInfo = {};
	destImageInfo.imageType = vk::ImageType::e2D;
	destImageInfo.format = vk::Format::eR8G8B8A8Unorm;
	destImageInfo.extent.width = m_SwapchainExtent.width;
	destImageInfo.extent.height = m_SwapchainExtent.height;
	destImageInfo.extent.depth = 1;
	destImageInfo.arrayLayers = 1;
	destImageInfo.mipLevels = 1;
	destImageInfo.initialLayout = vk::ImageLayout::eUndefined;
	destImageInfo.samples = vk::SampleCountFlagBits::e1;
	destImageInfo.tiling = vk::ImageTiling::eLinear;
	destImageInfo.usage = vk::ImageUsageFlagBits::eTransferDst;

	vk::raii::Image destImage(m_Device, destImageInfo);
	vk::MemoryRequirements memoryReqs = destImage.getMemoryRequirements();
	vk::MemoryAllocateInfo memoryAllocInfo = {};
	memoryAllocInfo.allocationSize = memoryReqs.size;

	auto memoryTypeIndex =
	  GetMemoryType(memoryReqs.memoryTypeBits,
					vk::MemoryPropertyFlagBits::eHostVisible |
					  vk::MemoryPropertyFlagBits::eHostCoherent,
					m_PhysicalDevice.getMemoryProperties());
	if (!memoryTypeIndex.has_value()) {
		Fail();
	}

	memoryAllocInfo.memoryTypeIndex = *memoryTypeIndex;
	auto destImageMemory = m_Device.allocateMemory(memoryAllocInfo);
	destImage.bindMemory(destImageMemory, 0);

	vk::CommandBufferAllocateInfo copyBufferInfo = {};
	copyBufferInfo.level = vk::CommandBufferLevel::ePrimary;
	copyBufferInfo.commandPool = m_CommandPool;
	copyBufferInfo.commandBufferCount = 1;
	vk::raii::CommandBuffer copyBuffer =
	  std::move(m_Device.allocateCommandBuffers(copyBufferInfo)[0]);

	copyBuffer.begin({});

	vk::ImageMemoryBarrier barrier = {};
	barrier.srcAccessMask = vk::AccessFlagBits::eNone;
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.oldLayout = vk::ImageLayout::eUndefined;
	barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.image = destImage;
	barrier.subresourceRange =
	  vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });

	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead;
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
	barrier.oldLayout = vk::ImageLayout::ePresentSrcKHR;
	barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.image = sourceImage;

	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });

	if (supportsBlitting) {
		vk::Offset3D blitSize = {};
		blitSize.x = m_SwapchainExtent.width;
		blitSize.y = m_SwapchainExtent.height;
		blitSize.z = 1;

		vk::ImageBlit blitRegion = {};
		blitRegion.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		blitRegion.srcSubresource.layerCount = 1;
		blitRegion.srcOffsets[1] = blitSize;
		blitRegion.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		blitRegion.dstSubresource.layerCount = 1;
		blitRegion.dstOffsets[1] = blitSize;

		copyBuffer.blitImage(sourceImage,
							 vk::ImageLayout::eTransferSrcOptimal,
							 destImage,
							 vk::ImageLayout::eTransferDstOptimal,
							 { blitRegion },
							 vk::Filter::eNearest);
	} else {
		vk::ImageCopy copyRegion = {};
		copyRegion.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.extent.width = m_SwapchainExtent.width;
		copyRegion.extent.height = m_SwapchainExtent.height;
		copyRegion.extent.depth = 1;

		copyBuffer.copyImage(sourceImage,
							 vk::ImageLayout::eTransferSrcOptimal,
							 destImage,
							 vk::ImageLayout::eTransferDstOptimal,
							 { copyRegion });
	}

	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.image = destImage;

	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });

	barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
	barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
	barrier.image = sourceImage;

	copyBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });
	copyBuffer.end();

	vk::SubmitInfo submitInfo = {};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &(*copyBuffer);

	vk::FenceCreateInfo fenceInfo = {};
	vk::raii::Fence fence(m_Device, fenceInfo);
	m_GraphicsQueue.submit({ submitInfo }, fence);
	ThrowIfFail(m_Device.waitForFences({ fence }, VK_TRUE, Timeout));

	vk::ImageSubresource subresource{ vk::ImageAspectFlagBits::eColor, 0, 0 };
	vk::SubresourceLayout subresourceLayout =
	  destImage.getSubresourceLayout(subresource);

	vk::MemoryMapInfo memoryMapInfo = {};
	memoryMapInfo.memory = destImageMemory;
	memoryMapInfo.size = VK_WHOLE_SIZE;

	uint8_t* data = nullptr;
	ThrowIfFail(vkMapMemory(*m_Device,
							*destImageMemory,
							0,
							VK_WHOLE_SIZE,
							0,
							reinterpret_cast<void**>(&data)));

	RageSurface* surface = CreateSurface(m_SwapchainExtent.width,
										 m_SwapchainExtent.height,
										 32,
										 0x000000ff,
										 0x0000ff00,
										 0x00ff0000,
										 0xff000000);

	for (size_t i = 0; i < 4LLU * surface->w * surface->h; i++) {
		// set alpha to 255 because it broke otherwise for some reason :(
		surface->pixels[i] = ((i + 1) % 4) ? data[i] : 255;
	}

	vkUnmapMemory(*m_Device, *destImageMemory);

	return surface;
}

RendererVK::~RendererVK()
{
	for (auto& [handle, texture] : m_Textures) {
		DestroyTexture(texture);
	}

	// WHAT
	vkDeviceWaitIdle(static_cast<VkDevice>(static_cast<vk::Device>(m_Device)));
}

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
RendererVK::InitVulkanState()
{
	vkb::InstanceBuilder builder;
	auto instanceResult =
	  builder
#ifdef _DEBUG || DEBUG
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
		.enable_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)
		.build();
	if (!instanceResult) {
		Fail();
	}

	m_Instance = vk::raii::Instance(m_Context, instanceResult->instance);
#ifdef _DEBUG || DEBUG
	m_DebugMessenger = vk::raii::DebugUtilsMessengerEXT(
	  m_Instance, instanceResult->debug_messenger);
#endif

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

	vk::SurfaceFormatKHR format = {};
	{
		std::vector<vk::SurfaceFormatKHR> formats =
		  m_PhysicalDevice.getSurfaceFormatsKHR(m_Surface);
		for (size_t i = 0; i < formats.size(); i++) {
			bool spaceOk =
			  formats[i].colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
			bool formatOk = (formats[i].format == vk::Format::eR8G8B8A8Unorm)
			  /* ||			(formats[i].format == vk::Format::eB8G8R8A8Unorm)*/;
			if (spaceOk && formatOk) {
				format = formats[i];
				break;
			}
		}

		if (format.format == vk::Format::eUndefined) {
			throw std::runtime_error(":(");
		}
	}

	vk::SwapchainCreateInfoKHR swapChainCreateInfo{};
	swapChainCreateInfo.surface = *m_Surface;
	swapChainCreateInfo.minImageCount = FramesInFlight;
	swapChainCreateInfo.imageFormat = ImageFormat;
	swapChainCreateInfo.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
	swapChainCreateInfo.imageExtent = m_SwapchainExtent;
	swapChainCreateInfo.imageArrayLayers = 1;
	swapChainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
									 vk::ImageUsageFlagBits::eTransferSrc;

	swapChainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
	swapChainCreateInfo.preTransform = surfaceCapabilities.currentTransform;

	vk::CompositeAlphaFlagBitsKHR compositeAlpha =
	  vk::CompositeAlphaFlagBitsKHR::eInherit;
	const auto scaFlags = surfaceCapabilities.supportedCompositeAlpha;
	if (scaFlags & vk::CompositeAlphaFlagBitsKHR::eOpaque) {
		compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	} else if (scaFlags & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied) {
		compositeAlpha = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
	}

	swapChainCreateInfo.compositeAlpha = compositeAlpha;
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
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(
		  2,
		  vk::DescriptorType::eCombinedImageSampler,
		  Texture::MaxSlots,
		  vk::ShaderStageFlagBits::eFragment)
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

	// we don't actually need any vertex info since we're reading stuffs from
	// the storage buffer
	vk::PipelineVertexInputStateCreateInfo vertexInfo = {};

	vk::GraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.pNext = &pipelineRenderingCreateInfo;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pVertexInputState = &vertexInfo;
	pipelineInfo.layout = m_PipelineLayout;
	pipelineInfo.renderPass = nullptr;

	m_GraphicsPipeline = vk::raii::Pipeline(m_Device, nullptr, pipelineInfo);
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

	m_CommandBuffers[m_CurrentFrame].pipelineBarrier2(dependencyInfo);
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
RendererVK::RecordCommands(uint32_t imageIndex, uint32_t indexCount)
{
	m_CommandBuffers[m_CurrentFrame].begin({});
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

	m_CommandBuffers[m_CurrentFrame].beginRendering(renderingInfo);

	m_CommandBuffers[m_CurrentFrame].bindPipeline(
	  vk::PipelineBindPoint::eGraphics, *m_GraphicsPipeline);

	m_CommandBuffers[m_CurrentFrame].bindDescriptorSets(
	  vk::PipelineBindPoint::eGraphics,
	  *m_PipelineLayout,
	  0,
	  { *m_DescriptorSets[m_CurrentFrame] },
	  nullptr);

	m_CommandBuffers[m_CurrentFrame].setViewport(
	  0,
	  vk::Viewport(0.0f,
				   static_cast<float>(m_SwapchainExtent.height),
				   static_cast<float>(m_SwapchainExtent.width),
				   -static_cast<float>(m_SwapchainExtent.height),
				   0.0f,
				   1.0f));
	m_CommandBuffers[m_CurrentFrame].setScissor(
	  0, vk::Rect2D(vk::Offset2D(0, 1), m_SwapchainExtent));

	m_CommandBuffers[m_CurrentFrame].bindIndexBuffer(
	  m_IndexBuffer[m_CurrentFrame].Get(), 0, vk::IndexType::eUint32);

	if (indexCount > 0) {
		m_CommandBuffers[m_CurrentFrame].drawIndexed(indexCount, 1, 0, 0, 0);
	}

	m_CommandBuffers[m_CurrentFrame].endRendering();

	TransitionImageLayout(imageIndex,
						  vk::ImageLayout::eColorAttachmentOptimal,
						  vk::ImageLayout::ePresentSrcKHR,
						  vk::AccessFlagBits2::eColorAttachmentWrite,
						  {},
						  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
						  vk::PipelineStageFlagBits2::eBottomOfPipe);
	m_CommandBuffers[m_CurrentFrame].end();
}

void
RendererVK::InitBatchBuffers()
{
	uint32_t textureDims = GetMaxTextureSize();

	VmaAllocationCreateInfo textureAllocInfo = {};
	textureAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	textureAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	textureAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	vk::BufferCreateInfo textureInfo = {};
	textureInfo.size =
	  (vk::DeviceSize)textureDims * textureDims * sizeof(uint32_t);
	textureInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
	m_TextureBuffer.Init(m_Allocator, textureInfo, textureAllocInfo);

	vk::DescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
	poolSizes[0].descriptorCount = 2 * FramesInFlight;
	poolSizes[1].type = vk::DescriptorType::eCombinedImageSampler;
	poolSizes[1].descriptorCount = Texture::MaxSlots * FramesInFlight;

	vk::DescriptorPoolCreateInfo poolInfo = {};
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	poolInfo.maxSets = FramesInFlight;
	m_DescriptorPool = vk::raii::DescriptorPool(m_Device, poolInfo);

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

	vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);
	m_DescriptorSetLayout = vk::raii::DescriptorSetLayout(m_Device, layoutInfo);

	std::vector<vk::DescriptorSetLayout> layouts(FramesInFlight,
												 *m_DescriptorSetLayout);

	vk::DescriptorSetAllocateInfo allocInfo(
	  *m_DescriptorPool, FramesInFlight, layouts.data());
	m_DescriptorSets = m_Device.allocateDescriptorSets(allocInfo);

	for (int i = 0; i < FramesInFlight; i++) {
		vk::BufferCreateInfo vertexBufferInfo{};
		vertexBufferInfo.size = sizeof(Display::Vertex) * MaxDrawCount;
		vertexBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo vertexAllocInfo = {};
		vertexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		vertexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		m_VertexBuffer[i].Init(
		  m_Allocator, vertexBufferInfo, vertexAllocInfo);

		vk::BufferCreateInfo indexBufferInfo{};
		indexBufferInfo.size = sizeof(uint32_t) * 5 * MaxDrawCount;
		indexBufferInfo.usage = vk::BufferUsageFlagBits::eIndexBuffer;
		VmaAllocationCreateInfo indexAllocInfo = {};
		indexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		indexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		m_IndexBuffer[i].Init(m_Allocator, indexBufferInfo, indexAllocInfo);

		vk::BufferCreateInfo matrixBufferInfo{};
		matrixBufferInfo.size = sizeof(Display::MatrixState) * MaxDrawCount;
		matrixBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;

		VmaAllocationCreateInfo matrixAllocInfo = {};
		matrixAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		matrixAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		m_MatrixStateBuffer[i].Init(
		  m_Allocator, matrixBufferInfo, matrixAllocInfo);

		vk::DescriptorBufferInfo triangleInfo(
		  m_VertexBuffer[i].Get(), 0, VK_WHOLE_SIZE);
		vk::DescriptorBufferInfo matrixInfo(
		  m_MatrixStateBuffer[i].Get(), 0, VK_WHOLE_SIZE);

		std::vector<vk::WriteDescriptorSet> writes = {
			vk::WriteDescriptorSet(m_DescriptorSets[i],
								   0,
								   0,
								   1,
								   vk::DescriptorType::eStorageBuffer,
								   nullptr,
								   &triangleInfo,
								   nullptr),
			vk::WriteDescriptorSet(m_DescriptorSets[i],
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
}

void
RendererVK::UpdateBatchBuffers(Display::CommandBatcher& batcher)
{
	if (!batcher.m_VertexBuffer.empty()) {
		std::map<std::pair<uint8_t, intptr_t>, int> textureLocation;
		std::array<vk::DescriptorImageInfo, Texture::MaxSlots> textureInfo;

		for (auto& info : textureInfo) {
			info.sampler = m_Samplers[0];
			info.imageView = m_Textures[0].view;
			info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}

		for (auto& vertex : batcher.m_VertexBuffer) {
			const auto& renderState =
			  batcher.m_RenderStateBuffer[vertex.TextureIndex];

			if (!renderState.textureHandle) {
				vertex.TextureIndex = 0;
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
				vertex.TextureIndex = it->second;
			} else {
				assert(textureLocation.size() < Texture::MaxSlots);
				textureInfo[textureLocation.size()].sampler =
				  m_Samplers[textureSettings];
				textureInfo[textureLocation.size()].imageView =
				  m_Textures[renderState.textureHandle].view;

				vertex.TextureIndex = textureLocation.size() + 1;
				textureLocation.emplace_hint(
				  it,
				  std::make_pair(
					std::make_pair(textureSettings, renderState.textureHandle),
					vertex.TextureIndex));
			}
		}

		vk::WriteDescriptorSet writeDescriptor = {};
		writeDescriptor.dstSet = m_DescriptorSets[m_CurrentFrame];
		writeDescriptor.dstBinding = 2;
		writeDescriptor.descriptorCount = Texture::MaxSlots;
		writeDescriptor.descriptorType =
		  vk::DescriptorType::eCombinedImageSampler;
		writeDescriptor.pImageInfo = textureInfo.data();

		m_Device.updateDescriptorSets({ writeDescriptor }, {});

		std::memcpy(m_VertexBuffer[m_CurrentFrame].GetMappedData(),
					batcher.m_VertexBuffer.data(),
					sizeof(Display::Vertex) * batcher.m_VertexBuffer.size());

		std::memcpy(m_IndexBuffer[m_CurrentFrame].GetMappedData(),
					batcher.m_IndexBuffer.data(),
					sizeof(uint32_t) * batcher.m_IndexBuffer.size());
	}

	if (!batcher.m_MatrixStateBuffer.empty()) {
		std::memcpy(m_MatrixStateBuffer[m_CurrentFrame].GetMappedData(),
					batcher.m_MatrixStateBuffer.data(),
					sizeof(Display::MatrixState) *
					  batcher.m_MatrixStateBuffer.size());
	}
}

int
RendererVK::GetMaxTextureSize()
{
	return std::min(
	  4096u, m_PhysicalDevice.getProperties().limits.maxImageDimension2D);
}

void
RendererVK::DestroyTexture(Texture& texture)
{
	if (texture.image) {
		vmaDestroyImage(m_Allocator, texture.image, texture.allocation);
	}
	if (texture.view) {
		vkDestroyImageView(*m_Device, texture.view, nullptr);
	}

	texture = {};
}

void
RendererVK::InitTextureSamplers()
{
	vk::PhysicalDeviceProperties properties = m_PhysicalDevice.getProperties();
	vk::SamplerCreateInfo samplerInfo = {};
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
	samplerInfo.anisotropyEnable = vk::True;
	samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
	samplerInfo.compareEnable = vk::False;
	samplerInfo.compareOp = vk::CompareOp::eAlways;

	for (size_t i = 0; i < m_Samplers.size(); i++) {
		samplerInfo.magFilter =
		  (i & Texture::Filtering) ? vk::Filter::eLinear : vk::Filter::eNearest;
		samplerInfo.minFilter =
		  (i & Texture::Filtering) ? vk::Filter::eLinear : vk::Filter::eNearest;
		samplerInfo.addressModeU = (i & Texture::Wrapping)
									 ? vk::SamplerAddressMode::eRepeat
									 : vk::SamplerAddressMode::eClampToBorder;
		samplerInfo.addressModeV = (i & Texture::Wrapping)
									 ? vk::SamplerAddressMode::eRepeat
									 : vk::SamplerAddressMode::eClampToBorder;
		samplerInfo.addressModeW = (i & Texture::Wrapping)
									 ? vk::SamplerAddressMode::eRepeat
									 : vk::SamplerAddressMode::eClampToBorder;
		m_Samplers[i] = vk::raii::Sampler(m_Device, samplerInfo);
	}

	RageSurface* img =
	  CreateSurface(1, 1, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	CreateTexture(img);
}

void
RendererVK::ResolutionChanged()
{
	m_SwapchainIsInvalid = true;
}
