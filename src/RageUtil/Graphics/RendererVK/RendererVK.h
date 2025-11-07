#ifndef RENDERER_VULKAN_H
#define RENDERER_VULKAN_H

#include "RageUtil/Graphics/Display/Renderer.h"
#include "RageUtil/Graphics/Display/TextureCommand.h"

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>
#include <array>
#include "VkUtils.h"

struct FrameData
{
	VkCommandPool CommandPool;
	VkCommandBuffer MainCommandBuffer;

	VkSemaphore SwapchainSemaphore;
	VkSemaphore RenderSemaphore;
	VkFence RenderFence;
	DeletionQueue InfoDeletion;
};

constexpr size_t FRAME_OVERLAP = 2;

class RendererVK : public Display::Renderer
{
  public:
	std::string GetApiDescription() const override;
	void InitializeRenderer(const VideoModeParams& p) override;
	void OnRender(const ActualVideoModeParams* p,
				  const Display::CommandBatcher& batcher) override;
	bool IsD3DInternal() override;
	intptr_t PushTextureCommand(
	  const Display::TextureCommand& command) override;

	~RendererVK() override;

  private:
	VkInstance m_Instance;
	VkDebugUtilsMessengerEXT m_DebugMessenger;
	VkPhysicalDevice m_GPU;
	VkDevice m_Device;
	VkSurfaceKHR m_Surface;

	VkSwapchainKHR m_Swapchain;
	VkFormat m_SwapchainImageFormat;
	std::vector<VkImage> m_SwapchainImages;
	std::vector<VkImageView> m_SwapchainImageViews;
	VkExtent2D m_SwapchainExtent;

	void InitVulkan();
	void InitSwapchain(const VideoModeParams& p);
	void InitCommands();
	void InitSyncStructures();

	void CreateSwapchain(size_t width, size_t height);
	void DestroySwapchain();

	std::array<FrameData, FRAME_OVERLAP> m_Frames;
	FrameData& GetCurrentFrame();
	size_t m_FrameNumber = 0;

	VkQueue m_GraphicsQueue;
	uint32_t m_GraphicsQueueFamily;

	DeletionQueue m_MainDeletionQueue;
	VmaAllocator m_Allocator;

	void HandleDrawCommands(VkCommandBuffer buffer,
							VkImage image,
							uint32_t drawCount,
							const ActualVideoModeParams* p);

	VkBuffer m_IndirectCommands = VK_NULL_HANDLE;
	VkDeviceMemory m_IndirectCommandMemory = VK_NULL_HANDLE;

	VkBuffer m_IndirectCommandArguments = VK_NULL_HANDLE;
	VkDeviceMemory m_IndirectCommandArgumentMemory = VK_NULL_HANDLE;

	VkBuffer m_SpriteVertices = VK_NULL_HANDLE;
	VkDeviceMemory m_SpriteVertexMemory = VK_NULL_HANDLE;

	VkBuffer m_RenderStates = VK_NULL_HANDLE;
	VkDeviceMemory m_RenderStateMemory = VK_NULL_HANDLE;

	VkBuffer m_MatrixStates = VK_NULL_HANDLE;
	VkDeviceMemory m_MatrixStateMemory = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_BufferDescriptorLayout;
	VkDescriptorSet m_BufferDescriptorSet;
	VkDescriptorPool m_DescriptorPool;
	VkPipelineLayout m_PipelineLayout;
	VkPipeline m_GraphicsPipeline;

	void InitBufferLayout();
	void InitPipelineLayout();
	void CreateDescriptorPool();
	void CreateDescriptorSet();
	void InitGraphicsPipeline();
	VkPipelineVertexInputStateCreateInfo GetSpriteVertexInfo();

	void InitInternalBuffers();
	void UpdateInternalBuffers(const Display::CommandBatcher& batcher);

	VkRenderPass m_RenderPass;
	std::vector<VkFramebuffer> m_Framebuffers;
	void InitRenderPass();
	void InitFramebuffers();
};

#endif
