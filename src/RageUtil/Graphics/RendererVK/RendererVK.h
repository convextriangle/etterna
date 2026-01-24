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
	void ResolutionChanged() override;

	~RendererVK() override;

  private:
	int GetMaxTextureSize();
};

#endif
