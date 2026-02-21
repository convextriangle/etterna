#ifndef DISPLAY_COMMAND_BATCHER_H
#define DISPLAY_COMMAND_BATCHER_H

#include "Vertex.h"
#include "RenderState.h"
#include <queue>
#include <string>

namespace Display {

struct RenderTargetCommand
{
	intptr_t RenderTarget;
	bool PreserveTexture;
	size_t DrawIndexOffset;
};

class CommandBatcher
{
  public:
	void InsertRenderStateCommand(RenderState renderState);
	void InsertSpriteDrawCommand(DrawMode drawMode,
								 MatrixState&& matrixState,
								 const RageSpriteVertex* vertexData,
								 int vertexCount);
	void InsertCompiledGeometryDrawCommand(DrawMode drawMode,
										   MatrixState&& matrixState,
										   const RageCompiledGeometry* p,
										   int iMeshIndex);
	void Clear();

	std::vector<Vertex> m_VertexBuffer;
	std::vector<uint32_t> m_IndexBuffer;
	std::vector<RenderState> m_RenderStateBuffer;
	std::vector<MatrixState> m_MatrixStateBuffer;
	std::vector<RenderTargetCommand> m_RenderTargetCommands;
};

} // namespace Display

#endif
