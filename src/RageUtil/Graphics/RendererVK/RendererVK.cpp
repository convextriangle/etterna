#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include "Core/Services/Locator.hpp"
#include <source_location>
#include <format>
#include <vulkan/vulkan_win32.h>
#include "VkUtils.h"

constexpr uint64_t Timeout = 1000'000'000;

std::string
RendererVK::GetApiDescription() const
{
	return "Vulkan";
}

static void
ThrowIfFail(
  VkResult result,
  const std::source_location location = std::source_location::current())
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

static void
Fail(const std::source_location location = std::source_location::current())
{
	const std::string message =
	  std::format("RendererVK failed at {}:{} in function {}",
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

void
RendererVK::StartLoadingPipeline()
{
	GraphicsWindow::Initialize(false);
	InitVulkan();
}

void
RendererVK::FinishLoadingPipeline(const VideoModeParams& p)
{
	InitSwapchain(p);
	InitCommands();
	InitSyncStructures();
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

	TransitionImage(buffer,
					m_SwapchainImages[swapchainImageIndex],
					VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_GENERAL);

	VkClearColorValue clearValue;
	float flash = std::abs(std::sin(m_FrameNumber / 120.f));
	clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

	VkImageSubresourceRange clearRange =
	  GetImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);

	vkCmdClearColorImage(buffer,
						 m_SwapchainImages[swapchainImageIndex],
						 VK_IMAGE_LAYOUT_GENERAL,
						 &clearValue,
						 1,
						 &clearRange);

	TransitionImage(buffer,
					m_SwapchainImages[swapchainImageIndex],
					VK_IMAGE_LAYOUT_GENERAL,
					VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	ThrowIfFail(vkEndCommandBuffer(buffer));

	auto bufferInfo = GetCommandBufferSubmitInfo(buffer);

	auto waitInfo = GetSemaphoreSubmitInfo(
	  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
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
	}

	DestroySwapchain();
	vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
	vkDestroyDevice(m_Device, nullptr);

	vkb::destroy_debug_utils_messenger(m_Instance, m_DebugMessenger);
	vkDestroyInstance(m_Instance, nullptr);
}

void
RendererVK::InitVulkan()
{
	vkb::InstanceBuilder builder;

	auto instanceResult = builder.request_validation_layers(true)
							.use_default_debug_messenger()
							.require_api_version(1, 3, 0)
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

	VkPhysicalDeviceVulkan13Features vk13Features{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
	};
	vk13Features.dynamicRendering = true;
	vk13Features.synchronization2 = true;

	VkPhysicalDeviceVulkan12Features vk12Features{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
	};
	vk12Features.bufferDeviceAddress = true;
	vk12Features.descriptorIndexing = true;

	vkb::PhysicalDeviceSelector selector(*instanceResult);
	auto physicalDevice = selector.set_minimum_version(1, 3)
							.set_required_features_13(vk13Features)
							.set_required_features_12(vk12Features)
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
