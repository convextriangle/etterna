#include "RendererVK.h"
#include "archutils/Win32/GraphicsWindow.h"

std::string RendererVK::GetApiDescription() const
{
	return "Vulkan";
}

void RendererVK::StartLoadingPipeline()
{
	GraphicsWindow::Initialize(true);
}

void RendererVK::FinishLoadingPipeline(const VideoModeParams& p)
{
}

void RendererVK::LoadAssets(const VideoModeParams& p)
{
}

void RendererVK::OnRender(const ActualVideoModeParams* p, const Display::CommandBatcher& batcher)
{
}

bool RendererVK::IsD3DInternal()
{
    return false;
}

intptr_t RendererVK::PushTextureCommand(const Display::TextureCommand& command)
{
    return intptr_t();
}
