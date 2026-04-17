#ifndef NOMINMAX // >:3
#define NOMINMAX
#endif
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifdef __unix__
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#define VMA_IMPLEMENTATION
#include "RendererVK.h"

#include <numbers>
#include <RageUtil/File/RageFileManager.h>
#include <RageUtil/Misc/RageMath.h>
#include <vulkan/vulkan_beta.h>
#include "RenderTargetVK.h"
#include "PlatformUtils.h"

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
					 const DisplayAdapter::CommandBatcher& batcher)
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
		DISPLAY->FrameLimitBeforeVsync();

		const auto beforePresent = std::chrono::steady_clock::now();
		result = m_PresentQueue.presentKHR(presentInfoKHR);
		const auto afterPresent = std::chrono::steady_clock::now();
		DISPLAY->SetPresentTime(afterPresent - beforePresent);

		DISPLAY->FrameLimitAfterVsync(
		  DISPLAY->GetActualVideoModeParams()->rate);
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
	texture.width = img->w;
	texture.height = img->h;

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
		barrier.srcAccessMask = vk::AccessFlags();
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
								 ? vk::PipelineStageFlagBits::eAllGraphics
								 : vk::PipelineStageFlagBits::eTopOfPipe,
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
							   vk::PipelineStageFlagBits::eAllGraphics,
							   {},
							   {},
							   {},
							   { barrier });

	copyBuffer.end();

	vk::SubmitInfo submitInfo = {};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &(*copyBuffer);

	vk::FenceCreateInfo fenceInfo;
	vk::raii::Fence fence(m_Device, fenceInfo);
	m_GraphicsQueue.submit({ submitInfo }, fence);
	ThrowIfFail(m_Device.waitForFences({ fence }, VK_TRUE, Timeout));

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

	Texture emptyTexture = m_Textures[0];
	for (auto& [handle, texture] : m_Textures) {
		if (handle == 0) {
			continue;
		}

		DestroyTexture(texture);
		m_Textures.erase(handle);
		m_EmptyTextureSlots.insert(handle);
	}

	m_Textures.clear();

	m_Textures[0] = emptyTexture;
	for (int i = 0; i < FramesInFlight; i++) {
		m_PendingTextureUpdates[i] = true;
	}
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
		Locator::getLogger()->error("RendererVK: failed to screenshot (can't "
									"find memory type for image creation)");
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

	if (m_DepthImage != nullptr) {
		m_DepthView = nullptr;
		vmaDestroyImage(m_Allocator, m_DepthImage, m_DepthAllocation);
		m_DepthImage = nullptr;
		m_DepthAllocation = nullptr;
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
		if (m_ShaderScratchBuffer[i].buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_Allocator,
							 m_ShaderScratchBuffer[i].buffer,
							 m_ShaderScratchBuffer[i].allocation);
			m_ShaderScratchBuffer[i].buffer = VK_NULL_HANDLE;
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
	switch (messageSeverity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: {
			Locator::getLogger()->trace("RendererVK debug callback: {}",
										pCallbackData->pMessage);
			break;
		}
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: {
			Locator::getLogger()->warn("RendererVK debug callback: {}",
									   pCallbackData->pMessage);
			break;
		}
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT: {
			Locator::getLogger()->error("RendererVK debug callback: {}",
										pCallbackData->pMessage);
			break;
		}
		default: {
			Locator::getLogger()->info("RendererVK debug callback: {}",
									   pCallbackData->pMessage);
			break;
		}
	}

	return VK_FALSE;
}

void
RendererVK::InitVulkanState()
{
	auto instanceResult = CreateInstance(VulkanDebugCallback);
	if (!instanceResult) {
		Locator::getLogger()->fatal("RendererVK: instance creation failed - {}",
									GetDetailedErrorString(instanceResult));
		Fail();
	}

	m_Instance = vk::raii::Instance(m_Context, instanceResult->instance);
#ifdef VKDEBUG
	m_DebugMessenger = vk::raii::DebugUtilsMessengerEXT(
	  m_Instance, instanceResult->debug_messenger);
#endif

	m_Surface = CreateSurfaceKHR(m_Instance);

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
	vk12Features.scalarBlockLayout = vk::True;

	VkPhysicalDeviceFeatures vkFeatures = {};
	vkFeatures.samplerAnisotropy = vk::True;
	vkFeatures.logicOp = vk::True;
	vkFeatures.shaderInt64 = vk::True;

	vkb::PhysicalDeviceSelector selector(*instanceResult);
	auto physicalDeviceResult =
	  selector.set_minimum_version(1, 3)
		.set_required_features_13(vk13Features)
		.set_required_features_12(vk12Features)
		.set_required_features(vkFeatures)
		.set_surface(static_cast<vk::SurfaceKHR>(m_Surface))
		.add_required_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
#ifdef __APPLE__
		.add_required_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
#endif
		.select();
	if (!physicalDeviceResult) {
		Locator::getLogger()->fatal(
		  "RendererVK: physical device creation failed - {}",
		  GetDetailedErrorString(physicalDeviceResult));
		Fail();
	}

	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{};
	dynamicState3Features.sType =
	  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
	dynamicState3Features.extendedDynamicState3ColorBlendEnable = VK_TRUE;
	dynamicState3Features.extendedDynamicState3ColorBlendEquation = VK_TRUE;
	dynamicState3Features.extendedDynamicState3ColorWriteMask = VK_TRUE;

	vkb::DeviceBuilder deviceBuilder(*physicalDeviceResult);
	auto deviceResult = deviceBuilder.add_pNext(&dynamicState3Features).build();
	if (!deviceResult) {
		Locator::getLogger()->fatal(
		  "RendererVK: device creation failed - {}",
		  GetDetailedErrorString(physicalDeviceResult));
		Fail();
	}

	m_PhysicalDevice = vk::raii::PhysicalDevice(
	  m_Instance, physicalDeviceResult->physical_device);
	m_Device = vk::raii::Device(m_PhysicalDevice, deviceResult->device);

	Locator::getLogger()->debug("RendererVK: selected GPU: {}",
								physicalDeviceResult->name);

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

	const std::vector<vk::Format> depthFormats{ vk::Format::eD32SfloatS8Uint,
												vk::Format::eD24UnormS8Uint };
	for (const auto& format : depthFormats) {
		auto props = m_PhysicalDevice.getFormatProperties2(format);
		if (props.formatProperties.optimalTilingFeatures &
			vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
			m_DepthFormat = format;
			break;
		}
	}
}

void
RendererVK::InitSwapchain(const VideoModeParams& p)
{
	vkb::SwapchainBuilder swapchainBuilder(
	  *m_PhysicalDevice, *m_Device, *m_Surface);

	swapchainBuilder.set_desired_min_image_count(FramesInFlight)
	  .set_desired_format(
		{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
	  .set_desired_format(
		{ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
	  .set_desired_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
	  .set_desired_extent(p.width, p.height)
	  .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
							 VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
	  .set_clipped(true);

	auto caps = *m_PhysicalDevice.getSurfaceCapabilitiesKHR(*m_Surface);
	swapchainBuilder.set_pre_transform_flags(caps.currentTransform);

	VkCompositeAlphaFlagBitsKHR compositeAlpha =
	  VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & compositeAlpha)) {
		if (caps.supportedCompositeAlpha &
			VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
			compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		else
			compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	}
	swapchainBuilder.set_composite_alpha_flags(compositeAlpha);

#ifdef _WIN32
	VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = {
		VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT
	};
	fullScreenInfo.fullScreenExclusive =
	  p.bWindowIsFullscreenBorderless ? VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
									  : VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
	swapchainBuilder.add_pNext(&fullScreenInfo);
#endif

	auto swapchain_ret = swapchainBuilder.build();
	if (!swapchain_ret) {
		Locator::getLogger()->fatal(
		  "RendererVK: swapchain creation failed - {}",
		  GetDetailedErrorString(swapchain_ret));
		Fail();
	}

	vkb::Swapchain vkbSwapchain = swapchain_ret.value();

	m_Swapchain = vk::raii::SwapchainKHR(m_Device, vkbSwapchain.swapchain);
	m_SwapchainImages = m_Swapchain.getImages();
	m_ImageFormat = static_cast<vk::Format>(vkbSwapchain.image_format);
	m_SwapchainExtent =
	  vk::Extent2D(vkbSwapchain.extent.width, vkbSwapchain.extent.height);

	vk::ImageCreateInfo depthImageInfo = {};
	depthImageInfo.imageType = vk::ImageType::e2D;
	depthImageInfo.format = m_DepthFormat;
	depthImageInfo.extent =
	  vk::Extent3D(vkbSwapchain.extent.width, vkbSwapchain.extent.height, 1);
	depthImageInfo.mipLevels = 1;
	depthImageInfo.arrayLayers = 1;
	depthImageInfo.samples = vk::SampleCountFlagBits::e1;
	depthImageInfo.tiling = vk::ImageTiling::eOptimal;
	depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	depthImageInfo.initialLayout = vk::ImageLayout::eUndefined;

	VmaAllocationCreateInfo depthAllocInfo = {};
	depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	ThrowIfFail(vmaCreateImage(m_Allocator,
							   &*depthImageInfo,
							   &depthAllocInfo,
							   &m_DepthImage,
							   &m_DepthAllocation,
							   nullptr));

	vk::ImageViewCreateInfo depthViewInfo = {};
	depthViewInfo.image = m_DepthImage;
	depthViewInfo.viewType = vk::ImageViewType::e2D;
	depthViewInfo.format = m_DepthFormat;
	vk::ImageSubresourceRange subRange = {};
	subRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	subRange.levelCount = 1;
	subRange.layerCount = 1;
	depthViewInfo.subresourceRange = subRange;
	m_DepthView = vk::raii::ImageView(m_Device, depthViewInfo);
}

void
RendererVK::RecreateSwapchain(const VideoModeParams& p)
{
	m_Device.waitIdle();

	CleanupSwapchain();
	InitSwapchain(p);
	InitImageViews();
	InitSyncStructures();
}

void
RendererVK::CleanupSwapchain()
{
	m_SwapchainImageViews.clear();
	m_Swapchain = nullptr;

	if (m_DepthImage != nullptr) {
		m_DepthView = nullptr;
		vmaDestroyImage(m_Allocator, m_DepthImage, m_DepthAllocation);
		m_DepthImage = nullptr;
		m_DepthAllocation = nullptr;
	}
}

void
RendererVK::InitImageViews()
{
	m_SwapchainImageViews.clear();
	vk::ImageViewCreateInfo createInfo{};
	createInfo.viewType = vk::ImageViewType::e2D;
	createInfo.format = m_ImageFormat;
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
	return {
		vk::DescriptorSetLayoutBinding(0,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(1,
									   vk::DescriptorType::eStorageBuffer,
									   1,
									   vk::ShaderStageFlagBits::eVertex),
		vk::DescriptorSetLayoutBinding(2,
									   vk::DescriptorType::eSampledImage,
									   GetMaxTextureCount(),
									   vk::ShaderStageFlagBits::eAllGraphics),
		vk::DescriptorSetLayoutBinding(3,
									   vk::DescriptorType::eSampler,
									   Texture::PossibleSamplerCount,
									   vk::ShaderStageFlagBits::eAllGraphics)
	};
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
						   const DisplayAdapter::CommandBatcher& batcher)
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
					: vk::PipelineStageFlagBits2::eAllGraphics,
		  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		  buffer);

		if (!swapchain) {
			m_Textures[node.RenderTarget].currentLayout =
			  vk::ImageLayout::eColorAttachmentOptimal;
		}

		vk::RenderingAttachmentInfo colorInfo = {};
		colorInfo.imageView = view;
		colorInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		colorInfo.storeOp = vk::AttachmentStoreOp::eStore;
		if (node.PreserveRenderTarget && !swapchain) {
			colorInfo.loadOp = vk::AttachmentLoadOp::eLoad;
		} else {
			colorInfo.loadOp = vk::AttachmentLoadOp::eClear;
			colorInfo.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f);
		}

		vk::RenderingAttachmentInfo depthInfo = {};
		depthInfo.imageView = m_DepthView;
		depthInfo.imageLayout = vk::ImageLayout::eAttachmentOptimal;
		depthInfo.loadOp = vk::AttachmentLoadOp::eClear;
		depthInfo.storeOp = vk::AttachmentStoreOp::eDontCare;
		depthInfo.clearValue = vk::ClearDepthStencilValue(1.0f, 0);

		vk::RenderingInfo renderInfo = {};
		renderInfo.renderArea =
		  vk::Rect2D{ { 0, 0 }, { extent.width, extent.height } };
		renderInfo.layerCount = 1;
		renderInfo.colorAttachmentCount = 1;
		renderInfo.pColorAttachments = &colorInfo;
		renderInfo.pDepthAttachment = &depthInfo;

		buffer.setScissor(
		  0,
		  vk::Rect2D(vk::Offset2D(0, 0),
					 vk::Extent2D(extent.width, extent.height)));
		buffer.setViewport(0,
						   vk::Viewport(0.0f,
										static_cast<float>(extent.height),
										static_cast<float>(extent.width),
										-static_cast<float>(extent.height),
										0.0f,
										1.0f));

		buffer.beginRendering(renderInfo);

		for (auto& call : node.DrawCalls) {
			buffer.setDepthTestEnable(
			  call.DepthTestMode != ZTEST_OFF ? VK_TRUE : VK_FALSE);
			buffer.setDepthWriteEnable(
			  call.DepthWriteEnabled ? VK_TRUE : VK_FALSE);

			vk::CompareOp depthCompareOp = vk::CompareOp::eAlways;
			switch (call.DepthTestMode) {
				case ZTEST_OFF: {
					break;
				}
				case ZTEST_WRITE_ON_PASS: {
					depthCompareOp = vk::CompareOp::eLessOrEqual;
					break;
				}
				case ZTEST_WRITE_ON_FAIL: {
					depthCompareOp = vk::CompareOp::eGreater;
					break;
				}
				default: {
					Locator::getLogger()->error(
					  "Invalid ZTestMode encountered: {}",
					  call.DepthTestMode);
					Fail();
				}
			}
			buffer.setDepthCompareOp(vk::CompareOp::eLessOrEqual);

			SetBlendMode(call.BlendingMode, buffer);

			if (call.Settings.GraphicsPipeline != currentPipeline) {
				currentPipeline = call.Settings.GraphicsPipeline;
				buffer.bindPipeline(
				  vk::PipelineBindPoint::eGraphics,
				  m_Pipelines[currentPipeline].GraphicsPipeline);
			}

			uint64_t vertexArg =
			  call.Settings.VertexShaderArg == UINT64_MAX
				? 0
				: m_ShaderScratchBuffer[m_CurrentFrame].gpuAddress +
					call.Settings.VertexShaderArg;

			buffer.pushConstants<uint64_t>(*m_Pipelines[0].PipelineLayout,
										   vk::ShaderStageFlagBits::eVertex |
											 vk::ShaderStageFlagBits::eFragment,
										   0,
										   { vertexArg });

			uint64_t fragArg =
			  call.Settings.FragShaderArg == UINT64_MAX
				? 0
				: m_ShaderScratchBuffer[m_CurrentFrame].gpuAddress +
					call.Settings.FragShaderArg;

			buffer.pushConstants<uint64_t>(*m_Pipelines[0].PipelineLayout,
										   vk::ShaderStageFlagBits::eVertex |
											 vk::ShaderStageFlagBits::eFragment,
										   sizeof(uint64_t),
										   { fragArg });

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
					: vk::PipelineStageFlagBits2::eAllGraphics,
		  buffer);

		if (!swapchain) {
			m_Textures[node.RenderTarget].currentLayout =
			  vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	}

	buffer.end();
}

void
RendererVK::SetBlendMode(BlendMode mode, vk::raii::CommandBuffer& buffer)
{
	vk::Bool32 enableBlending = VK_TRUE;
	vk::ColorComponentFlags colorWriteMask =
	  vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	vk::ColorBlendEquationEXT blendEquation{};
	blendEquation.colorBlendOp = vk::BlendOp::eAdd;
	blendEquation.alphaBlendOp = vk::BlendOp::eAdd;

	switch (mode) {
		case BLEND_NORMAL: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstColorBlendFactor =
			  vk::BlendFactor::eOneMinusSrcAlpha;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstAlphaBlendFactor =
			  vk::BlendFactor::eOneMinusSrcAlpha;
			break;
		}

		case BLEND_ADD: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eOne;
			break;
		}

		case BLEND_SUBTRACT: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eZero;
			break;
		}

		case BLEND_MODULATE: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eSrcColor;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eSrcColor;
			break;
		}

		case BLEND_COPY_SRC: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eZero;
			break;
		}

		case BLEND_ALPHA_MASK: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
			break;
		}

		case BLEND_ALPHA_KNOCK_OUT: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstAlphaBlendFactor =
			  vk::BlendFactor::eOneMinusSrcAlpha;
			break;
		}

		case BLEND_ALPHA_MULTIPLY: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eZero;
			break;
		}

		case BLEND_WEIGHTED_MULTIPLY: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eDstColor;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eSrcColor;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eDstColor;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eSrcColor;
			break;
		}

		case BLEND_INVERT_DEST: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
			blendEquation.colorBlendOp = vk::BlendOp::eSubtract;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eOne;
			blendEquation.alphaBlendOp = vk::BlendOp::eSubtract;
			break;
		}

		case BLEND_NO_EFFECT: {
			blendEquation.srcColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.dstColorBlendFactor = vk::BlendFactor::eZero;
			blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
			blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eOne;
			break;
		}

		default: {
			Locator::getLogger()->error("Invalid BlendMode: {}", mode);
			Fail();
		}
	}

	buffer.setColorBlendEnableEXT(0, { enableBlending });
	buffer.setColorWriteMaskEXT(0, { colorWriteMask });
	buffer.setColorBlendEquationEXT(0, { blendEquation });
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
	poolSizes[0].descriptorCount = 2 * FramesInFlight;
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
		vertexBufferInfo.size = sizeof(DisplayAdapter::Vertex) * MaxDrawCount;
		vertexBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo vertexAllocInfo = {};
		vertexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		vertexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		vertexAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_VertexBuffer[i].Init(m_Allocator, vertexBufferInfo, vertexAllocInfo);

		vk::BufferCreateInfo indexBufferInfo{};
		indexBufferInfo.size = sizeof(uint32_t) * 5 * MaxDrawCount;
		indexBufferInfo.usage = vk::BufferUsageFlagBits::eIndexBuffer;
		VmaAllocationCreateInfo indexAllocInfo = {};
		indexAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		indexAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		indexAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_IndexBuffer[i].Init(m_Allocator, indexBufferInfo, indexAllocInfo);

		vk::BufferCreateInfo matrixBufferInfo{};
		matrixBufferInfo.size =
		  sizeof(DisplayAdapter::MatrixState) * MaxDrawCount;
		matrixBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer;
		VmaAllocationCreateInfo matrixAllocInfo = {};
		matrixAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		matrixAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		matrixAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_MatrixStateBuffer[i].Init(
		  m_Allocator, matrixBufferInfo, matrixAllocInfo);

		vk::BufferCreateInfo scratchBufferInfo{};
		scratchBufferInfo.size = sizeof(uint8_t) * 1'000'000;
		scratchBufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer |
								  vk::BufferUsageFlagBits::eShaderDeviceAddress;
		VmaAllocationCreateInfo scratchAllocInfo = {};
		scratchAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		scratchAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		scratchAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		m_ShaderScratchBuffer[i].Init(
		  m_Allocator, scratchBufferInfo, scratchAllocInfo);
		vk::BufferDeviceAddressInfo scratchAddressInfo = {};
		scratchAddressInfo.buffer = m_ShaderScratchBuffer[i].buffer;
		m_ShaderScratchBuffer[i].gpuAddress =
		  m_Device.getBufferAddress(scratchAddressInfo);

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
RendererVK::UpdateBatchBuffers(const DisplayAdapter::CommandBatcher& batcher)
{
	if (m_PendingTextureUpdates[m_CurrentFrame]) {
		m_PendingTextureUpdates[m_CurrentFrame] = false;

		std::vector<vk::DescriptorImageInfo> textureInfo(GetMaxTextureCount());
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
		writeDescriptor.dstBinding = 2;
		writeDescriptor.descriptorCount = textureInfo.size();
		writeDescriptor.descriptorType = vk::DescriptorType::eSampledImage;
		writeDescriptor.pImageInfo = textureInfo.data();

		m_Device.updateDescriptorSets({ writeDescriptor }, {});
	}

	if (batcher.m_VertexBuffer.empty()) {
		return;
	}

	std::memcpy(m_VertexBuffer[m_CurrentFrame].GetMappedData(),
				batcher.m_VertexBuffer.data(),
				sizeof(DisplayAdapter::Vertex) * batcher.m_VertexBuffer.size());

	std::memcpy(m_IndexBuffer[m_CurrentFrame].GetMappedData(),
				batcher.m_IndexBuffer.data(),
				sizeof(uint32_t) * batcher.m_IndexBuffer.size());

	std::memcpy(m_ShaderScratchBuffer[m_CurrentFrame].GetMappedData(),
				batcher.m_ShaderScratchBuffer.data(),
				sizeof(uint8_t) * batcher.m_ShaderScratchBuffer.size());

	std::memcpy(m_MatrixStateBuffer[m_CurrentFrame].GetMappedData(),
				batcher.m_MatrixStateBuffer.data(),
				sizeof(DisplayAdapter::MatrixState) *
				  batcher.m_MatrixStateBuffer.size());
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
		writeDescriptor.dstBinding = 3;
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
	texture.width = width;
	texture.height = height;

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

	auto fragmentShader =
	  LoadShaderFromFile(fragmentShaderPath, m_Device, ShaderType_Fragment);
	auto vertexShader =
	  LoadShaderFromFile(vertexShaderPath, m_Device, ShaderType_Vertex);

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
								  vk::DynamicState::eScissor,
								  vk::DynamicState::eDepthWriteEnable,
								  vk::DynamicState::eDepthTestEnable,
								  vk::DynamicState::eDepthCompareOp,
								  vk::DynamicState::eColorBlendEnableEXT,
								  vk::DynamicState::eColorBlendEquationEXT,
								  vk::DynamicState::eColorWriteMaskEXT };

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

	if (m_DescriptorSetLayout == nullptr) {
		std::vector<vk::DescriptorSetLayoutBinding> bindings =
		  GetDescriptorBindings();
		vk::DescriptorSetLayoutCreateInfo layoutInfo({}, bindings);

		m_DescriptorSetLayout =
		  vk::raii::DescriptorSetLayout(m_Device, layoutInfo);
	}

	std::array<vk::PushConstantRange, 1> pushConstants = {};
	pushConstants[0].size = sizeof(uint64_t) * 2;
	pushConstants[0].stageFlags =
	  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

	vk::PipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &*m_DescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = pushConstants.data();

	info.PipelineLayout =
	  vk::raii::PipelineLayout(m_Device, pipelineLayoutInfo);

	vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.blendEnable = VK_FALSE;
	colorBlendAttachment.colorWriteMask =
	  vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	  vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	vk::PipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	vk::PipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;

	vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{};
	pipelineRenderingCreateInfo.colorAttachmentCount = 1;
	pipelineRenderingCreateInfo.pColorAttachmentFormats = &m_ImageFormat;
	pipelineRenderingCreateInfo.depthAttachmentFormat = m_DepthFormat;

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
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pVertexInputState = &vertexInfo;
	pipelineInfo.layout = info.PipelineLayout;
	pipelineInfo.renderPass = nullptr;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;

	info.GraphicsPipeline = vk::raii::Pipeline(m_Device, nullptr, pipelineInfo);

	m_Pipelines.push_back(std::move(info));
	return static_cast<intptr_t>(m_Pipelines.size() - 1);
}

void
RendererVK::TryVideoMode(const VideoModeParams& params)
{
	m_Device.waitIdle();

	m_Surface = CreateSurfaceKHR(m_Instance);
	RecreateSwapchain(params);
}
