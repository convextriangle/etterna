#ifndef DISPLAY_RENDER_NODE_H
#define DISPLAY_RENDER_NODE_H

#include <cstdint>
#include <vector>
#include <set>

struct DrawCall
{
	intptr_t GraphicsPipeline = 0;
	size_t IndexOffset = 0;
	size_t IndexCount = 0;
};

struct RenderNode
{
	intptr_t RenderTarget = 0;
	bool PreserveRenderTarget = false;
	std::vector<DrawCall> DrawCalls;
	std::set<size_t> Dependencies;
};

#endif
