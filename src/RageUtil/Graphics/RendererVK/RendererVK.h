#ifndef RENDERER_VULKAN_H
#define RENDERER_VULKAN_H

#include "RageUtil/Graphics/Display/Renderer.h"
#include "RageUtil/Graphics/Display/TextureCommand.h"

class RendererVK : public Display::Renderer
{
	// Inherited via Renderer
	std::string GetApiDescription() const override;
	void StartLoadingPipeline() override;
	void FinishLoadingPipeline(const VideoModeParams& p) override;
	void LoadAssets(const VideoModeParams& p) override;
	void OnRender(const ActualVideoModeParams* p, const Display::CommandBatcher& batcher) override;
	bool IsD3DInternal() override;
	intptr_t PushTextureCommand(const Display::TextureCommand& command) override;
};

#endif
