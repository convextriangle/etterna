#ifndef DISPLAY_RENDER_NODE_H
#define DISPLAY_RENDER_NODE_H

#include <cstdint>
#include <vector>
#include <set>

struct PipelineSettings
{
	intptr_t GraphicsPipeline = 0;
	intptr_t VertexShaderArg = 0;
	intptr_t FragShaderArg = 0;
};

struct DrawCall
{
	PipelineSettings Settings = {};
	size_t IndexOffset = 0;
	size_t IndexCount = 0;
};

struct RenderNode
{
	intptr_t RenderTarget = 0;
	bool PreserveRenderTarget = false;
	std::vector<DrawCall> DrawCalls;
	std::vector<size_t> Dependencies;
};

#endif
