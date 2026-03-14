#ifndef NOMINMAX // >:3
#define NOMINMAX
#endif

#define VMA_IMPLEMENTATION
#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include <numbers>
#include <RageUtil/File/RageFileManager.h>
#include <RageUtil/Misc/RageMath.h>
#include <vulkan/vulkan_beta.h>
#include "RenderTargetVK.h"

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
	InitTextures();
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
	RecordCommands(imageIndex, batcher);

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
RendererVK::CreateTexture(RageSurface* img, bool RGBA8)
{
	assert(m_EmptyTextureSlots.size());
	intptr_t currentHandle = *m_EmptyTextureSlots.begin();
	m_EmptyTextureSlots.erase(currentHandle);

	Texture texture = {};
	texture.width = power_of_two(img->w);
	texture.height = power_of_two(img->h);

	VmaAllocationCreateInfo allocCreateInfo = {};
	allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format =
	  RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
	imageInfo.extent = { texture.width, texture.height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.usage =
	  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	VkImage imagePtr = nullptr;
	VmaAllocationInfo allocInfo = {};
	ThrowIfFail(vmaCreateImage(m_Allocator,
							   &imageInfo,
							   &allocCreateInfo,
							   &imagePtr,
							   &texture.allocation,
							   &allocInfo));
	texture.image = imagePtr;

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = texture.image;
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format =
	  RGBA8 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eB8G8R8A8Unorm;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	texture.view = (*m_Device).createImageView(viewInfo);
	texture.currentLayout = vk::ImageLayout::eUndefined;
	m_Textures.insert({ currentHandle, texture });

	UpdateTexture(currentHandle, img, 0, 0, img->w, img->h);

	for (int i = 0; i < FramesInFlight; i++) {
		m_PendingTextureUpdates[i] = true;
	}
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
	if (texture.initialized) {
		barrier.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
		barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	} else {
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.oldLayout = vk::ImageLayout::eUndefined;
		barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
	}
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture.image;
	barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	copyBuffer.pipelineBarrier(texture.initialized
								 ? vk::PipelineStageFlagBits::eFragmentShader
								 : vk::PipelineStageFlagBits::eHost,
							   vk::PipelineStageFlagBits::eTransfer,
							   {},
							   {},
							   {},
							   { barrier });

	vk::BufferImageCopy imageCopy = {};
	imageCopy.imageExtent =
	  vk::Extent3D{ (uint32_t)width, (uint32_t)height, 1 };
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

	texture.initialized = true;
	texture.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

void
RendererVK::DeleteTexture(intptr_t handle)
{
	m_GraphicsQueue.waitIdle();

	DestroyTexture(m_Textures[handle]);
	m_Textures.erase(handle);
	m_EmptyTextureSlots.insert(handle);

	for (int i = 0; i < FramesInFlight; i++) {
		m_PendingTextureUpdates[i] = true;
	}
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

intptr_t
RendererVK::CreateRenderTarget(const RenderTargetParam& param,
							   int& iTextureWidthOut,
							   int& iTextureHeightOut)
{
	RenderTargetVK target = {};
	target.Create(param, iTextureWidthOut, iTextureHeightOut);
	target.m_Texture = CreateRenderTargetTexture(target.GetParam().iWidth,
												 target.GetParam().iHeight);
	return target.m_Texture;
}

RendererVK::~RendererVK()
{
	if (m_Device != nullptr) {
		m_Device.waitIdle();
	}

	for (auto& [handle, texture] : m_Textures) {
		DestroyTexture(texture);
	}

	if (m_TextureBuffer.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(
		  m_Allocator, m_TextureBuffer.buffer, m_TextureBuffer.allocation);
		m_TextureBuffer.buffer = VK_NULL_HANDLE;
	}

	for (int i = 0; i < FramesInFlight; i++) {
		if (m_VertexBuffer[i].buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_Allocator,
							 m_VertexBuffer[i].buffer,
							 m_VertexBuffer[i].allocation);
			m_VertexBuffer[i].buffer = VK_NULL_HANDLE;
		}
		if (m_IndexBuffer[i].buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_Allocator,
							 m_IndexBuffer[i].buffer,
							 m_IndexBuffer[i].allocation);
			m_IndexBuffer[i].buffer = VK_NULL_HANDLE;
		}
		if (m_MatrixStateBuffer[i].buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_Allocator,
							 m_MatrixStateBuffer[i].buffer,
							 m_MatrixStateBuffer[i].allocation);
			m_MatrixStateBuffer[i].buffer = VK_NULL_HANDLE;
		}
		if (m_DrawSettingsBuffer[i].buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_Allocator,
							 m_DrawSettingsBuffer[i].buffer,
							 m_DrawSettingsBuffer[i].allocation);
			m_DrawSettingsBuffer[i].buffer = VK_NULL_HANDLE;
		}
	}

	// likely the only thing that needs to be cleaned up manually (because
	// descriptor pool should exist on descriptor sets' deletion)
	m_DescriptorSets.clear();

	if (m_Allocator != nullptr) {
		vmaDestroyAllocator(m_Allocator);
	}
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
#ifdef __APPLE__
		.enable_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
		.enable_extension(
		  VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)
#endif
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
	vk13Features.dynamicRendering = vk::True;
	vk13Features.synchronization2 = vk::True;

	VkPhysicalDeviceVulkan12Features vk12Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
	};
	vk12Features.bufferDeviceAddress = vk::True;
	vk12Features.descriptorIndexing = vk::True;
	vk12Features.runtimeDescriptorArray = vk::True;
	vk12Features.shaderSampledImageArrayNonUniformIndexing = vk::True;

	VkPhysicalDeviceVulkan11Features vk11Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
	};
	vk11Features.shaderDrawParameters = true;

	VkPhysicalDeviceFeatures vkFeatures = {};
	vkFeatures.samplerAnisotropy = vk::True;
	vkFeatures.multiDrawIndirect = vk::True;
	vkFeatures.logicOp = vk::True;

	vkb::PhysicalDeviceSelector selector(*instanceResult);
	auto physicalDeviceResult =
	  selector.set_minimum_version(1, 3)
		.set_required_features_13(vk13Features)
		.set_required_features_12(vk12Features)
		.set_required_features_11(vk11Features)
		.set_required_features(vkFeatures)
		.set_surface(static_cast<vk::SurfaceKHR>(m_Surface))
#ifdef __APPLE__
		.add_required_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
#endif
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
			bool formatOk = (formats[i].format == vk::Format::eR8G8B8A8Unorm);
			if (spaceOk && formatOk) {
				format = formats[i];
				break;
			}
		}

		if (format.format == vk::Format::eUndefined) {
			Fail();
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
	CreateGraphicsPipeline(
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/vertex.glsl"),
	  FILEMAN->ResolvePath("Data/Shaders/Vulkan/fragment.glsl"));
}

std::vector<vk::DescriptorSetLayoutBinding>
RendererVK::GetDescriptorBindings()
{
	return { vk::DescriptorSetLayoutBinding(0,
											vk::DescriptorType::eStorageBuffer,
											1,
											vk::ShaderStageFlagBits::eVertex),
			 vk::DescriptorSetLayoutBinding(1,
											vk::DescriptorType::eStorageBuffer,
											1,
											vk::ShaderStageFlagBits::eVertex),
			 vk::DescriptorSetLayoutBinding(2,
											vk::DescriptorType::eStorageBuffer,
											1,
											vk::ShaderStageFlagBits::eVertex),
			 // technically someone might want to access textures in vertex
			 // shader, so switch to vk::ShaderStageFlagBits::eAllGraphics?
			 vk::DescriptorSetLayoutBinding(3,
											vk::DescriptorType::eSampledImage,
											GetMaxTextureCount(),
											vk::ShaderStageFlagBits::eFragment),
			 vk::DescriptorSetLayoutBinding(
			   4,
			   vk::DescriptorType::eSampler,
			   Texture::PossibleSamplerCount,
			   vk::ShaderStageFlagBits::eFragment) };
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
RendererVK::TransitionImageLayout(vk::Image& image,
								  vk::ImageLayout oldLayout,
								  vk::ImageLayout newLayout,
								  vk::AccessFlags2 srcAccessMask,
								  vk::AccessFlags2 dstAccessMask,
								  vk::PipelineStageFlags2 srcStageMask,
								  vk::PipelineStageFlags2 dstStageMask,
								  vk::raii::CommandBuffer& commandBuffer)
{
	if (oldLayout == newLayout) {
		return;
	}

	vk::ImageMemoryBarrier2 barrier{};
	barrier.srcStageMask = srcStageMask;
	barrier.srcAccessMask = srcAccessMask;
	barrier.dstStageMask = dstStageMask;
	barrier.dstAccessMask = dstAccessMask;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

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

	commandBuffer.pipelineBarrier2(dependencyInfo);
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
		m_InFlightFence.emplace_back(
		  m_Device, vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
	}
	for (size_t i = 0; i < m_SwapchainImages.size(); i++) {
		m_RenderFinishedSemaphore.emplace_back(m_Device,
											   vk::SemaphoreCreateInfo());
	}
}

void
RendererVK::RecordCommands(uint32_t imageIndex,
						   Display::CommandBatcher& batcher)
{
	auto& buffer = m_CommandBuffers[m_CurrentFrame];
	buffer.begin({});

	buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
							  *m_Pipelines[0].PipelineLayout,
							  0,
							  { *m_DescriptorSets[m_CurrentFrame] },
							  nullptr);
	buffer.bindIndexBuffer(
	  m_IndexBuffer[m_CurrentFrame].Get(), 0, vk::IndexType::eUint32);

	intptr_t currentPipeline = -1;

	for (auto& node : batcher.m_RenderNodes) {
		bool swapchain = node.RenderTarget == 0;

		auto image = swapchain ? m_SwapchainImages[imageIndex]
							   : m_Textures[node.RenderTarget].image;
		auto view = swapchain ? m_SwapchainImageViews[imageIndex]
							  : m_Textures[node.RenderTarget].view;
		auto extent = swapchain
						? m_SwapchainExtent
						: vk::Extent2D(m_Textures[node.RenderTarget].width,
									   m_Textures[node.RenderTarget].height);

		TransitionImageLayout(
		  image,
		  swapchain ? vk::ImageLayout::eUndefined
					: m_Textures[node.RenderTarget].currentLayout,
		  vk::ImageLayout::eColorAttachmentOptimal,
		  swapchain ? vk::AccessFlags2() : vk::AccessFlagBits2::eShaderRead,
		  vk::AccessFlagBits2::eColorAttachmentWrite |
			vk::AccessFlagBits2::eColorAttachmentRead,
		  swapchain ? vk::PipelineStageFlagBits2::eColorAttachmentOutput
					: vk::PipelineStageFlagBits2::eFragmentShader,
		  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		  buffer);

		if (!swapchain) {
			m_Textures[node.RenderTarget].currentLayout =
			  vk::ImageLayout::eColorAttachmentOptimal;
		}

		vk::RenderingAttachmentInfo colorInfo{};
		colorInfo.imageView = view;
		colorInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		colorInfo.storeOp = vk::AttachmentStoreOp::eStore;
		if (node.PreserveRenderTarget && !swapchain) {
			colorInfo.loadOp = vk::AttachmentLoadOp::eLoad;
		} else {
			colorInfo.loadOp = vk::AttachmentLoadOp::eClear;
			colorInfo.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f);
		}
		vk::RenderingInfo renderInfo{};
		renderInfo.renderArea =
		  vk::Rect2D{ { 0, 0 }, { extent.width, extent.height } };
		renderInfo.layerCount = 1;
		renderInfo.colorAttachmentCount = 1;
		renderInfo.pColorAttachments = &colorInfo;

		buffer.setViewport(0,
						   vk::Viewport(0.0f,
										static_cast<float>(extent.height),
										static_cast<float>(extent.width),
										-static_cast<float>(extent.height),
										0.0f,
										1.0f));
		buffer.setScissor(
		  0,
		  vk::Rect2D(vk::Offset2D(0, 0),
					 vk::Extent2D(extent.width, extent.height)));

		buffer.beginRendering(renderInfo);

		for (auto& call : node.DrawCalls) {
			if (call.Settings.GraphicsPipeline != currentPipeline) {
				currentPipeline = call.Settings.GraphicsPipeline;
				buffer.bindPipeline(
				  vk::PipelineBindPoint::eGraphics,
				  m_Pipelines[currentPipeline].GraphicsPipeline);
			}

			if (call.Settings.VertexShaderArg != 0) {
				buffer.pushConstants<intptr_t>(
				  *m_Pipelines[0].PipelineLayout,
				  vk::ShaderStageFlagBits::eVertex,
				  0,
				  { call.Settings.VertexShaderArg });
			}
			if (call.Settings.FragShaderArg != 0) {
				buffer.pushConstants<intptr_t>(
				  *m_Pipelines[0].PipelineLayout,
				  vk::ShaderStageFlagBits::eFragment,
				  sizeof(intptr_t),
				  { call.Settings.FragShaderArg });
			}

			buffer.drawIndexed(call.IndexCount, 1, call.IndexOffset, 0, 0);
		}

		buffer.endRendering();

		TransitionImageLayout(
		  image,
		  swapchain ? vk::ImageLayout::eColorAttachmentOptimal
					: m_Textures[node.RenderTarget].currentLayout,
		  swapchain ? vk::ImageLayout::ePresentSrcKHR
					: vk::ImageLayout::eShaderReadOnlyOptimal,
		  vk::AccessFlagBits2::eColorAttachmentWrite,
		  swapchain ? vk::AccessFlags2() : vk::AccessFlagBits2::eShaderRead,
		  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		  swapchain ? vk::PipelineStageFlagBits2::eBottomOfPipe
					: vk::PipelineStageFlagBits2::eFragmentShader,
		  buffer);

		if (!swapchain) {
			m_Textures[node.RenderTarget].currentLayout =
			  vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	}

	buffer.end();
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

	vk::DescriptorPoolSize poolSizes[3] = {};
	poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
	poolSizes[0].descriptorCount = 3 * FramesInFlight;
	poolSizes[1].type = vk::DescriptorType::eSampledImage;
	poolSizes[1].descriptorCount = GetMaxTextureCount() * FramesInFlight;
	poolSizes[2].type = vk::DescriptorType::eSampler;
	poolSizes[2].descriptorCount =
	  Texture::PossibleSamplerCount * FramesInFlight;

	vk::DescriptorPoolCreateInfo poolInfo = {};
	poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes = poolSizes;
	poolInfo.maxSets = FramesInFlight;
	m_DescriptorPool = vk::raii::DescriptorPool(m_Device, poolInfo);

	std::vector<vk::DescriptorSetLayoutBinding> bindings =
	  GetDescriptorBindings();
	vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);

	m_DescriptorSetLayout = vk::raii::DescriptorSetLayout(m_Device, layoutInfo);

	std::vector<vk::DescriptorSetLayout> layouts(FramesInFlight,
												 *m_DescriptorSetLayout);

	vk::DescriptorSetAllocateInfo allocInfo(
	  *m_DescriptorPool, FramesInFlight, layouts.data());
	m_DescriptorSets = m_Device.allocateDescriptorSets(allocInfo);

	for (int i = 0; i < FramesInFlight; i++) {
		vk::BufferCreateInfo vertexBufferInfo{};
		vertexBufferInfo.size = sizeof(RageSpriteVertex) * MaxDrawCount;
		vertexBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo vertexAllocInfo = {};
		vertexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		vertexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		vertexAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_VertexBuffer[i].Init(m_Allocator, vertexBufferInfo, vertexAllocInfo);

		vk::BufferCreateInfo drawSettingsInfo{};
		drawSettingsInfo.size =
		  sizeof(uint32_t) + sizeof(Display::DrawSettings) * MaxDrawCount;
		drawSettingsInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo drawSettingsAllocInfo = {};
		drawSettingsAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		drawSettingsAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		drawSettingsAllocInfo.requiredFlags =
		  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_DrawSettingsBuffer[i].Init(
		  m_Allocator, drawSettingsInfo, drawSettingsAllocInfo);

		vk::BufferCreateInfo indexBufferInfo{};
		indexBufferInfo.size = sizeof(uint32_t) * 5 * MaxDrawCount;
		indexBufferInfo.usage = vk::BufferUsageFlagBits::eIndexBuffer;
		VmaAllocationCreateInfo indexAllocInfo = {};
		indexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		indexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		indexAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_IndexBuffer[i].Init(m_Allocator, indexBufferInfo, indexAllocInfo);

		vk::BufferCreateInfo matrixBufferInfo{};
		matrixBufferInfo.size = sizeof(Display::MatrixState) * MaxDrawCount;
		matrixBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo matrixAllocInfo = {};
		matrixAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		matrixAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		matrixAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_MatrixStateBuffer[i].Init(
		  m_Allocator, matrixBufferInfo, matrixAllocInfo);

		vk::DescriptorBufferInfo triangleInfo(
		  m_VertexBuffer[i].Get(), 0, VK_WHOLE_SIZE);
		vk::DescriptorBufferInfo matrixInfo(
		  m_MatrixStateBuffer[i].Get(), 0, VK_WHOLE_SIZE);
		vk::DescriptorBufferInfo settingsInfo(
		  m_DrawSettingsBuffer[i].Get(), 0, VK_WHOLE_SIZE);

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
								   nullptr),
			vk::WriteDescriptorSet(m_DescriptorSets[i],
								   2,
								   0,
								   1,
								   vk::DescriptorType::eStorageBuffer,
								   nullptr,
								   &settingsInfo,
								   nullptr)
		};

		m_Device.updateDescriptorSets(writes, nullptr);
	}
}

void
RendererVK::UpdateBatchBuffers(Display::CommandBatcher& batcher)
{
	if (!batcher.m_VertexBuffer.empty()) {
		if (m_PendingTextureUpdates[m_CurrentFrame]) {
			m_PendingTextureUpdates[m_CurrentFrame] = false;

			std::vector<vk::DescriptorImageInfo> textureInfo(
			  GetMaxTextureCount());
			for (int i = 0; i < textureInfo.size(); i++) {
				textureInfo[i].imageLayout =
				  vk::ImageLayout::eShaderReadOnlyOptimal;

				if (m_EmptyTextureSlots.contains(i)) {
					textureInfo[i].imageView = m_Textures[0].view;
				} else {
					textureInfo[i].imageView = m_Textures[i].view;
				}
			}

			vk::WriteDescriptorSet writeDescriptor = {};
			writeDescriptor.dstSet = m_DescriptorSets[m_CurrentFrame];
			writeDescriptor.dstBinding = 3;
			writeDescriptor.descriptorCount = textureInfo.size();
			writeDescriptor.descriptorType = vk::DescriptorType::eSampledImage;
			writeDescriptor.pImageInfo = textureInfo.data();

			m_Device.updateDescriptorSets({ writeDescriptor }, {});
		}

		std::memcpy(m_VertexBuffer[m_CurrentFrame].GetMappedData(),
					batcher.m_VertexBuffer.data(),
					sizeof(RageSpriteVertex) * batcher.m_VertexBuffer.size());

		std::memcpy(m_IndexBuffer[m_CurrentFrame].GetMappedData(),
					batcher.m_IndexBuffer.data(),
					sizeof(uint32_t) * batcher.m_IndexBuffer.size());

		uint8_t* settingsBuffer =
		  (uint8_t*)m_DrawSettingsBuffer[m_CurrentFrame].GetMappedData();

		uint32_t settingsCount = batcher.m_DrawSettingsBuffer.size();
		std::memcpy(settingsBuffer, &settingsCount, sizeof(uint32_t));
		settingsBuffer += sizeof(uint32_t);

		std::memcpy(settingsBuffer,
					batcher.m_DrawSettingsBuffer.data(),
					sizeof(Display::DrawSettings) *
					  batcher.m_DrawSettingsBuffer.size());
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

int
RendererVK::GetMaxTextureCount()
{
	return std::min(
	  static_cast<size_t>(Texture::MaxTextures),
	  m_PhysicalDevice.getProperties().limits.maxDescriptorSetSampledImages /
		FramesInFlight);
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
RendererVK::InitTextures()
{
	vk::PhysicalDeviceProperties properties = m_PhysicalDevice.getProperties();
	vk::SamplerCreateInfo samplerInfo = {};
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
	samplerInfo.anisotropyEnable = vk::True;
	samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
	samplerInfo.compareEnable = vk::False;
	samplerInfo.compareOp = vk::CompareOp::eAlways;
	std::array<vk::DescriptorImageInfo, Texture::PossibleSamplerCount>
	  samplerImageInfo;

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
		m_Samplers[i] = vk::raii::Sampler(m_Device, samplerInfo);
		samplerImageInfo[i].sampler = m_Samplers[i];
	}

	for (int i = 0; i < FramesInFlight; i++) {
		vk::WriteDescriptorSet writeDescriptor = {};
		writeDescriptor.dstSet = m_DescriptorSets[i];
		writeDescriptor.dstBinding = 4;
		writeDescriptor.descriptorCount = Texture::PossibleSamplerCount;
		writeDescriptor.descriptorType = vk::DescriptorType::eSampler;
		writeDescriptor.pImageInfo = samplerImageInfo.data();

		m_Device.updateDescriptorSets({ writeDescriptor }, nullptr);
	}

	for (int i = 0; i < GetMaxTextureCount(); i++) {
		m_EmptyTextureSlots.insert(i);
	}

	RageSurface* img =
	  CreateSurface(1, 1, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	CreateTexture(img, true);
}

void
RendererVK::ResolutionChanged()
{
	m_SwapchainIsInvalid = true;
}

intptr_t
RendererVK::CreateRenderTargetTexture(int width, int height)
{
	assert(m_EmptyTextureSlots.size());
	intptr_t currentHandle = *m_EmptyTextureSlots.begin();
	m_EmptyTextureSlots.erase(currentHandle);

	Texture texture = {};
	texture.width = power_of_two(width);
	texture.height = power_of_two(height);

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
	  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	VkImage imagePtr = nullptr;
	VmaAllocationInfo allocInfo = {};
	ThrowIfFail(vmaCreateImage(m_Allocator,
							   &imageInfo,
							   &allocCreateInfo,
							   &imagePtr,
							   &texture.allocation,
							   &allocInfo));
	texture.image = imagePtr;

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = texture.image;
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = vk::Format::eR8G8B8A8Unorm;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	texture.view = (*m_Device).createImageView(viewInfo);
	texture.currentLayout = vk::ImageLayout::eUndefined;

	m_Textures.insert({ currentHandle, texture });

	return currentHandle;
}

intptr_t
RendererVK::CreateGraphicsPipeline(const std::string& vertexShaderPath,
								   const std::string& fragmentShaderPath)
{
	auto previousPipeline =
	  m_PipelineLookup.find({ vertexShaderPath, fragmentShaderPath });

	if (previousPipeline != m_PipelineLookup.end()) {
		return previousPipeline->second;
	} else {
		m_PipelineLookup[{ vertexShaderPath, fragmentShaderPath }] =
		  m_Pipelines.size();
	}

	PipelineInfo info = {};

	auto fragmentShader = LoadShaderFromFile(
	  fragmentShaderPath, m_Device, shaderc_glsl_fragment_shader);
	auto vertexShader = LoadShaderFromFile(
	  vertexShaderPath, m_Device, shaderc_glsl_vertex_shader);

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
	rasterizer.cullMode = vk::CullModeFlagBits::eBack;
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
	colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
	colorBlendAttachment.dstAlphaBlendFactor =
	  vk::BlendFactor::eOneMinusSrcAlpha;
	colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
	colorBlendAttachment.colorWriteMask =
	  vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	vk::PipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	if (m_DescriptorSetLayout == nullptr) {
		std::vector<vk::DescriptorSetLayoutBinding> bindings =
		  GetDescriptorBindings();
		vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);

		m_DescriptorSetLayout =
		  vk::raii::DescriptorSetLayout(m_Device, layoutInfo);
	}

	std::array<vk::PushConstantRange, 2> pushConstants = {};
	pushConstants[0].size = sizeof(intptr_t);
	pushConstants[0].stageFlags = vk::ShaderStageFlagBits::eVertex;
	pushConstants[1].offset = sizeof(intptr_t);
	pushConstants[1].size = sizeof(intptr_t);
	pushConstants[1].stageFlags = vk::ShaderStageFlagBits::eFragment;

	vk::PipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &*m_DescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 2;
	pipelineLayoutInfo.pPushConstantRanges = pushConstants.data();

	info.PipelineLayout =
	  vk::raii::PipelineLayout(m_Device, pipelineLayoutInfo);

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
	pipelineInfo.layout = info.PipelineLayout;
	pipelineInfo.renderPass = nullptr;

	info.GraphicsPipeline = vk::raii::Pipeline(m_Device, nullptr, pipelineInfo);

	m_Pipelines.push_back(std::move(info));
	return static_cast<intptr_t>(m_Pipelines.size() - 1);
}
