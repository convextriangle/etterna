#ifndef DISPLAY_RENDER_STATE_H
#define DISPLAY_RENDER_STATE_H

#include "RageUtil/Misc/RageTypes.h"
#include "RageUtil/Graphics/RageDisplay.h"

namespace Display
{

struct RenderState
{
    bool textureWrapping;
    bool textureFiltering;
    intptr_t textureHandle;

	bool operator==(RenderState& rhs);
};

// double-check for Display::CommandBatcher
static_assert(std::is_trivially_copyable_v<RenderState>);

} // namespace Display

#endif
