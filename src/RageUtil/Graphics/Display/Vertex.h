#ifndef DISPLAY_DRAW_COMMAND_H
#define DISPLAY_DRAW_COMMAND_H

#include "DrawMode.h"
#include "MatrixState.h"
#include "RageUtil/Graphics/RageDisplay.h"
#include "RageUtil/Misc/RageTypes.h"

namespace Display
{

struct Vertex
{
    RageSpriteVertex InnerData;
    uint32_t MatrixIndex;
    uint32_t TextureIndex;
	uint32_t SamplerIndex;
};

} // namespace Display

#endif
