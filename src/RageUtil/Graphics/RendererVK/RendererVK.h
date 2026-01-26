#ifndef RENDERER_VULKAN_H
#define RENDERER_VULKAN_H

#include "RageUtil/Graphics/Display/Renderer.h"

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>
#include <array>
#include "VkUtils.h"
#include "Texture.h"
#include "PersistentBuffer.h"

class RendererVK : public Display::Renderer
{
  public:
	RendererVK();
	std::string GetApiDescription() const override;
	void InitializeRenderer(const VideoModeParams& p) override;
	void OnRender(const ActualVideoModeParams* p,
				  Display::CommandBatcher& batcher) override;
	bool IsD3DInternal() override;
	intptr_t CreateTexture(RageSurface* img) override;
	void UpdateTexture(intptr_t textureHandle,
					   RageSurface* img,
					   int xOffset,
					   int yOffset,
					   int width,
					   int height) override;
	void DeleteTexture(intptr_t handle) override;
	void ClearAllTextures() override;

	~RendererVK() override;

  private:
	vk::raii::Context m_Context;
	vk::raii::Instance m_Instance = nullptr;
	vk::raii::DebugUtilsMessengerEXT m_DebugMessenger = nullptr;
	vk::raii::PhysicalDevice m_PhysicalDevice = nullptr;
	vk::raii::Device m_Device = nullptr;
	vk::raii::SurfaceKHR m_Surface = nullptr;
	vk::raii::Queue m_GraphicsQueue = nullptr;
	uint32_t m_GraphicsQueueFamily;
	vk::raii::Queue m_PresentQueue = nullptr;
	uint32_t m_PresentQueueFamily;
	VmaAllocator m_Allocator = nullptr;
	void InitVulkanState();

	vk::raii::SwapchainKHR m_Swapchain = nullptr;
	vk::Extent2D m_SwapchainExtent;
	std::vector<vk::Image> m_SwapchainImages;

	constexpr static vk::Format ImageFormat = vk::Format::eB8G8R8A8Unorm;

	bool m_SwapchainIsInvalid = false;
	void InitSwapchain(const VideoModeParams& p);
	void RecreateSwapchain(const VideoModeParams& p);
	void CleanupSwapchain();

	std::vector<vk::raii::ImageView> m_SwapchainImageViews;
	void InitImageViews();

	vk::raii::PipelineLayout m_PipelineLayout = nullptr;
	vk::raii::Pipeline m_GraphicsPipeline = nullptr;
	vk::raii::DescriptorSetLayout m_DescriptorSetLayout = nullptr;
	void InitGraphicsPipeline();

	vk::raii::CommandPool m_CommandPool = nullptr;
	void InitCommandPool();

	std::vector<vk::raii::CommandBuffer> m_CommandBuffers;
	void InitCommandBuffers();

	void TransitionImageLayout(uint32_t imageIndex,
							   vk::ImageLayout oldLayout,
							   vk::ImageLayout newLayout,
							   vk::AccessFlags2 srcAccessMask,
							   vk::AccessFlags2 dstAccessMask,
							   vk::PipelineStageFlags2 srcStageMask,
							   vk::PipelineStageFlags2 dstStageMask);

	std::vector<vk::raii::Semaphore> m_PresentCompleteSemaphore;
	std::vector<vk::raii::Semaphore> m_RenderFinishedSemaphore;
	std::vector<vk::raii::Fence> m_InFlightFence;
	uint32_t semaphoreIndex = 0;
	uint32_t currentFrame = 0;
	void InitSyncStructures();
	void RecordCommands(uint32_t imageIndex, uint32_t drawCount);

	constexpr static size_t FramesInFlight = 6;
	constexpr static size_t MaxDrawCount = 50'000;

	std::array<PersistentBuffer, FramesInFlight> m_TriangleBuffer;
	std::array<PersistentBuffer, FramesInFlight> m_MatrixStateBuffer;
	PersistentBuffer m_TextureBuffer;

	std::vector<vk::raii::DescriptorSet> m_DescriptorSets;
	vk::raii::DescriptorPool m_DescriptorPool = nullptr;

	void InitBatchBuffers();
	void UpdateBatchBuffers(Display::CommandBatcher& batcher);

	intptr_t m_TextureCounter = 0;
	std::unordered_map<intptr_t, Texture> m_Textures;
	int GetMaxTextureSize();
	void DestroyTexture(Texture& texture);

	std::array<vk::raii::Sampler, Texture::PossibleSamplerCount> m_Samplers;
	void InitTextureSamplers();
	void ResolutionChanged() override;
};

#endif
