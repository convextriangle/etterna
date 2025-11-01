#include "RendererVK.h"

// no penguin (For Now (TM))
#include "archutils/Win32/GraphicsWindow.h"
#include "Core/Services/Locator.hpp"
#include <source_location>
#include <format>

#define VOLK_IMPLEMENTATION
#include <Volk/volk.h>

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

void
RendererVK::StartLoadingPipeline()
{
	GraphicsWindow::Initialize(false);

	CreateVulkanInstance();
	LoadDebugMessenger();
	CreateSurface();
	PickPhysicalDevice();
	InitDevice();
	CreateSwapChain();
}

void
RendererVK::FinishLoadingPipeline(const VideoModeParams& p)
{
}

void
RendererVK::LoadAssets(const VideoModeParams& p)
{
}

void
RendererVK::OnRender(const ActualVideoModeParams* p,
					 const Display::CommandBatcher& batcher)
{
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
	vkDestroySwapchainKHR(m_Device, m_SwapChain, nullptr);
	vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
	vkDestroyDevice(m_Device, nullptr);

#ifndef NDEBUG
	vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
#endif
	vkDestroyInstance(m_Instance, nullptr);
}

static VkBool32
VkDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
				VkDebugUtilsMessageTypeFlagsEXT messageTypes,
				const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
				void* userData)
{
	std::string message = callbackData->pMessage;

	// waka laka (hi happy cat)
	if (message.substr(0, 16) != "Device Extension") {
		Locator::getLogger()->debug(message);
	}
	return VK_FALSE;
}

void
RendererVK::CreateVulkanInstance()
{
	ThrowIfFail(volkInitialize());

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.apiVersion = VK_API_VERSION_1_4;

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;

	const std::vector<const char*> validationLayer = {
		"VK_LAYER_KHRONOS_validation"
	};
	instanceInfo.enabledLayerCount = validationLayer.size();
	instanceInfo.ppEnabledLayerNames = validationLayer.data();

	std::vector<const char*> instanceExtensions = {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
	};

	instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
	instanceInfo.enabledExtensionCount = instanceExtensions.size();

	ThrowIfFail(vkCreateInstance(&instanceInfo, nullptr, &m_Instance));
	volkLoadInstance(m_Instance);
}

void
RendererVK::LoadDebugMessenger()
{
#ifndef NDEBUG
	VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo = {
		VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
	};

	debugMessengerInfo.messageSeverity =
	  VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
	  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
	  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
	  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

	debugMessengerInfo.messageType =
	  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
	  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
	  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

	debugMessengerInfo.pfnUserCallback = VkDebugCallback;
	vkCreateDebugUtilsMessengerEXT(
	  m_Instance, &debugMessengerInfo, 0, &m_DebugMessenger);
#endif
}

void
RendererVK::PickPhysicalDevice()
{
	uint32_t deviceCount = 0;
	ThrowIfFail(vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr));

	if (deviceCount == 0) {
		throw std::runtime_error("Failed to find devices with Vulkan support");
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

	for (const auto& device : devices) {
		if (IsDeviceSuitable(device)) {
			m_PhysicalDevice = device;
			break;
		}
	}

	if (m_PhysicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("Failed to find suitable device for Vulkan");
	}
}

bool
RendererVK::IsDeviceSuitable(VkPhysicalDevice device)
{
	const std::vector<const char*> deviceExtensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	uint32_t extensionCount = 0;
	vkEnumerateDeviceExtensionProperties(
	  device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);

	vkEnumerateDeviceExtensionProperties(
	  device, nullptr, &extensionCount, availableExtensions.data());

	std::set<std::string> extensionsToFind(deviceExtensions.begin(),
										   deviceExtensions.end());
	for (const auto& extension : availableExtensions) {
		extensionsToFind.erase(extension.extensionName);
	}

	auto families = FindQueueFamilies(device);
	auto info = QuerySwapChainSupport(device);
	return families.graphicsFamily.has_value() &&
		   families.presentFamily.has_value() && extensionsToFind.empty() &&
		   !info.formats.empty() && !info.presentModes.empty();
}

void
RendererVK::InitDevice()
{
	auto indices = FindQueueFamilies(m_PhysicalDevice);

	std::set<uint32_t> families = { indices.graphicsFamily.value(),
									indices.presentFamily.value() };

	float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueInfo;

	for (const auto& family : families) {
		VkDeviceQueueCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		createInfo.queueFamilyIndex = family;
		createInfo.queueCount = 1;
		createInfo.pQueuePriorities = &queuePriority;

		queueInfo.push_back(createInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures{};
	VkDeviceCreateInfo deviceInfo{};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.pQueueCreateInfos = queueInfo.data();
	deviceInfo.queueCreateInfoCount = queueInfo.size();
	deviceInfo.pEnabledFeatures = &deviceFeatures;

	const std::vector<const char*> deviceExtensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	deviceInfo.enabledExtensionCount = deviceExtensions.size();
	deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
	deviceInfo.enabledLayerCount = 0;

	ThrowIfFail(
	  vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device));

	vkGetDeviceQueue(
	  m_Device, indices.graphicsFamily.value(), 0, &m_GraphicsQueue);
	vkGetDeviceQueue(
	  m_Device, indices.presentFamily.value(), 0, &m_PresentQueue);
}

RendererVK::VkQueueFamilyIndices
RendererVK::FindQueueFamilies(VkPhysicalDevice device)
{
	VkQueueFamilyIndices indices = {};

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(
	  device, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(
	  device, &queueFamilyCount, queueFamilies.data());

	for (int i = 0; const auto& family : queueFamilies) {
		if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}

		VkBool32 supportsPresenting = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(
		  device, i, m_Surface, &supportsPresenting);
		if (supportsPresenting) {
			indices.graphicsFamily = i;
		}

		i++;
	}

	return indices;
}

void
RendererVK::CreateSurface()
{
#ifdef _WIN32
	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hwnd = GraphicsWindow::GetHwnd();
	createInfo.hinstance = GetModuleHandle(nullptr);

	ThrowIfFail(
	  vkCreateWin32SurfaceKHR(m_Instance, &createInfo, nullptr, &m_Surface));
#else
#error TODO
#endif
}

RendererVK::SwapChainSupportInfo
RendererVK::QuerySwapChainSupport(VkPhysicalDevice device)
{
	RendererVK::SwapChainSupportInfo info{};

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	  device, m_Surface, &info.capabilities);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(
	  device, m_Surface, &formatCount, nullptr);

	if (formatCount != 0) {
		info.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(
		  device, m_Surface, &formatCount, info.formats.data());
	}

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(
	  device, m_Surface, &presentModeCount, nullptr);
	if (presentModeCount != 0) {
		info.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(
		  device, m_Surface, &presentModeCount, info.presentModes.data());
	}

	return info;
}

VkSurfaceFormatKHR
RendererVK::ChooseSwapSurfaceFormat(
  const std::vector<VkSurfaceFormatKHR>& formats)
{
	for (const auto& format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
			format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return format;
		}
	}

	return formats[0];
}

VkPresentModeKHR
RendererVK::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& formats)
{
	for (const auto& format : formats) {
		if (format == VK_PRESENT_MODE_MAILBOX_KHR) {
			return format;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
RendererVK::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities)
{
	return capabilities.currentExtent;
}

void
RendererVK::CreateSwapChain()
{
	SwapChainSupportInfo info = QuerySwapChainSupport(m_PhysicalDevice);
	auto surfaceFormat = ChooseSwapSurfaceFormat(info.formats);
	auto presentMode = ChooseSwapPresentMode(info.presentModes);
	auto extent = ChooseSwapExtent(info.capabilities);

	uint32_t imageCount = info.capabilities.minImageCount + 1;
	if (info.capabilities.maxImageCount &&
		imageCount > info.capabilities.maxImageCount) {
		imageCount = info.capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_Surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	auto indices = FindQueueFamilies(m_PhysicalDevice);
	uint32_t indexArray[] = { indices.graphicsFamily.value(),
							  indices.presentFamily.value() };

	if (indices.graphicsFamily != indices.presentFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = indexArray;
	} else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.queueFamilyIndexCount = 0;
		createInfo.pQueueFamilyIndices = nullptr;
	}

	createInfo.preTransform = info.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	ThrowIfFail(
	  vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_SwapChain));
}
