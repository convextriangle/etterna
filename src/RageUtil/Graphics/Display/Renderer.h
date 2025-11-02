#ifndef DISPLAY_RENDERER_H
#define DISPLAY_RENDERER_H

#include <string>
#include "RageUtil/Graphics/RageDisplay.h"
#include "RageUtil/Graphics/Display/CommandBatcher.h"
#include "TextureCommand.h"

namespace Display {
class Renderer
{
  public:
	virtual ~Renderer() {}
    virtual [[nodiscard]] std::string GetApiDescription() const = 0;
	virtual void InitializeRenderer(const VideoModeParams& p) = 0;
	virtual void OnRender(const ActualVideoModeParams* p, const CommandBatcher& batcher) = 0;
	virtual bool IsD3DInternal() = 0;
	virtual intptr_t PushTextureCommand(const TextureCommand& command) = 0;
};
}

#endif
