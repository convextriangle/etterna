#ifndef DISPLAY_RENDER_STATE_H
#define DISPLAY_RENDER_STATE_H

#include "RageUtil/Misc/RageTypes.h"
#include "RageUtil/Graphics/RageDisplay.h"

namespace DisplayAdapter {

struct RenderState
{
	bool textureWrapping;
	bool textureFiltering;
	intptr_t textureHandle;
};

} // namespace DisplayAdapter

#endif
