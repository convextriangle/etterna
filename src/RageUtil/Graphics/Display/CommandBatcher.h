#ifndef DISPLAY_COMMAND_BATCHER_H
#define DISPLAY_COMMAND_BATCHER_H

#include "RenderState.h"
#include <queue>
#include <string>
#include "DrawMode.h"
#include "MatrixState.h"

namespace Display {

struct RenderTargetCommand
{
	intptr_t RenderTarget;
	bool PreserveTexture;
	size_t DrawIndexOffset;
};

struct PipelineChangeCommand
{
	intptr_t Pipeline;
	size_t DrawIndexOffset;
};

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
	void InsertPipelineChangeCommand(intptr_t pipeline, bool persist);
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
	void Clear();

	std::vector<RageSpriteVertex> m_VertexBuffer;
	std::vector<DrawSettings> m_DrawSettingsBuffer;
	std::vector<uint32_t> m_IndexBuffer;
	std::vector<MatrixState> m_MatrixStateBuffer;
	std::vector<RenderTargetCommand> m_RenderTargetCommands;
	std::vector<PipelineChangeCommand> m_PipelineCommands;
};

} // namespace Display

#endif
