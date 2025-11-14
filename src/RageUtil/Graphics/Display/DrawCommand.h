#ifndef DISPLAY_DRAW_COMMAND_H
#define DISPLAY_DRAW_COMMAND_H

#include "DrawMode.h"
#include "MatrixState.h"
#include "RageUtil/Graphics/RageDisplay.h"
#include "RageUtil/Misc/RageTypes.h"

namespace Display
{

// Based on DirectX12, but by chance matches Vulkan (lucky (^_^))
struct DrawCommand
{
	// VkDrawIndirectCommand::vertexCount
    uint32_t VertexCountPerInstance;

	// VkDrawIndirectCommand::instanceCount
    uint32_t InstanceCount;

	// VkDrawIndirectCommand::firstVertex
    uint32_t StartVertexLocation;

	// VkDrawIndirectCommand::firstInstance
    uint32_t StartInstanceLocation;
};

struct DrawCommandArgument
{
    uint32_t renderStateIndex; // hi hello :3
};

} // namespace Display

#endif
