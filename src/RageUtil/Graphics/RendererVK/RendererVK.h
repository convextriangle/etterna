#ifndef RENDERER_VULKAN_H
#define RENDERER_VULKAN_H

#include "RageUtil/Graphics/Display/Renderer.h"
#include "RageUtil/Graphics/Display/TextureCommand.h"

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vulkan/vulkan.h"
#include <optional>

class RendererVK : public Display::Renderer
{
  public:
	std::string GetApiDescription() const override;
	void StartLoadingPipeline() override;
	void FinishLoadingPipeline(const VideoModeParams& p) override;
	void LoadAssets(const VideoModeParams& p) override;
	void OnRender(const ActualVideoModeParams* p,
				  const Display::CommandBatcher& batcher) override;
	bool IsD3DInternal() override;
	intptr_t PushTextureCommand(
	  const Display::TextureCommand& command) override;

	~RendererVK() override;

  private:
	VkInstance m_Instance = VK_NULL_HANDLE;
	void CreateVulkanInstance();

#ifndef NDEBUG
	VkDebugUtilsMessengerEXT m_DebugMessenger;
	void LoadDebugMessenger();
#endif

	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	bool IsDeviceSuitable(VkPhysicalDevice device);
	void PickPhysicalDevice();

	VkDevice m_Device = VK_NULL_HANDLE;
	void InitDevice();

	struct VkQueueFamilyIndices
	{
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;
	};

	VkQueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);

	VkQueue m_GraphicsQueue;
	VkQueue m_PresentQueue;

	VkSurfaceKHR m_Surface;
	void CreateSurface();

	struct SwapChainSupportInfo
	{
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	SwapChainSupportInfo QuerySwapChainSupport(VkPhysicalDevice device);
	VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
	VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& formats);
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

	VkSwapchainKHR m_SwapChain;
	void CreateSwapChain();
};

#endif
