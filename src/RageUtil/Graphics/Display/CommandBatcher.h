#ifndef DISPLAY_COMMAND_BATCHER_H
#define DISPLAY_COMMAND_BATCHER_H

#include <queue>
#include <stack>
#include <string>
#include <map>
#include "DrawMode.h"
#include "MatrixState.h"
#include "RenderState.h"
#include "RenderNode.h"

namespace Display {

struct DrawSettings
{
	uint32_t FirstVertexIndex;
	uint32_t MatrixIndex;
	uint32_t TextureIndex;
	uint32_t SamplerIndex;
};

class CommandBatcher
{
  public:
	void InsertPipelineChangeCommand(intptr_t pipeline, intptr_t vertexShaderInfo, intptr_t fragShaderInfo, bool persist);
	void InsertRenderTargetCommand(intptr_t renderTarget, bool preserveTexture);
	void InsertSpriteDrawCommand(DrawMode drawMode,
								 MatrixState&& matrixState,
								 const RageSpriteVertex* vertexData,
								 int vertexCount,
								 const RenderState& renderState
					);
	void InsertCompiledGeometryDrawCommand(MatrixState&& matrixState,
										   const RageCompiledGeometry* p,
										   int iMeshIndex,
										   const RenderState& renderState);
	void HandleDrawCommand(int indexOffset, int indexCount, const RenderState& renderState);
	void Clear();
	void SortRenderNodes();
	void RenderNodeSearch(size_t nodeIndex);

	std::vector<RageSpriteVertex> m_VertexBuffer;
	std::vector<DrawSettings> m_DrawSettingsBuffer;
	std::vector<uint32_t> m_IndexBuffer;
	std::vector<MatrixState> m_MatrixStateBuffer;
	std::vector<RenderNode> m_RenderNodes;
	std::multimap<intptr_t, size_t> m_RenderTargetLookup;
	std::stack<PipelineSettings> m_PipelineStack;
	std::vector<size_t> m_SortedNodes;
	std::vector<bool> m_VisitedNodes;
	PipelineSettings m_CurrentPipeline = {};
};

} // namespace Display

#endif
