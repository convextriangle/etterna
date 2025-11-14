#include "CommandBatcher.h"
#include <cassert>

void
Display::CommandBatcher::InsertRenderStateCommand(RenderState renderState)
{
	m_RenderStateBuffer.push_back(renderState);
}

void
Display::CommandBatcher::InsertSpriteDrawCommand(
  DrawMode drawMode,
  MatrixState&& matrixState,
  const RageSpriteVertex* vertexData,
  int vertexCount)
{
	assert(drawMode != DrawMode::Invalid);
	assert(drawMode != DrawMode::CompiledGeometry);
	assert(m_RenderStateBuffer.size() >= 1 &&
		   "Rendering information must be set before drawing");

	DrawCommand command = {};

	command.StartVertexLocation = m_SpriteVertexBuffer.size();
	command.InstanceCount = 1;
	command.StartInstanceLocation = m_IndirectCommandBuffer.size();

	// -- changing draw mode in the middle of the queue would likely require
	// switching pipeline state objects
	//	  (pipeline objects are chonky)
	//	  and most of the ye olde draw modes aren't supported
	//    so just convert to a triangle list
	// -- unrolled loops look funny though
	// -- maybe this can be done on the GPU via mesh shaders and/or work graphs
	// but that's for unstable_vk_mintyfresh
	// -- and like two people have GPUs that support stuff like this so no thank
	// you
	switch (drawMode) {
		case DrawMode::Triangles: {
			std::copy(vertexData,
					  vertexData + vertexCount,
					  std::back_inserter(m_SpriteVertexBuffer));
			break;
		}
		case DrawMode::Quads: {
			for (size_t i = 0; i < vertexCount / 4; i++) {
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 0]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 2]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 2]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 3]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 4 + 0]);
			}

			break;
		}
		case DrawMode::QuadStrip: {
			for (size_t i = 0; i < (vertexCount - 2) / 2; i++) {
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 0]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 2]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 2]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 2 + 3]);
			}

			break;
		}
		case DrawMode::Fan: {
			assert(vertexCount >= 3);

			for (size_t i = 1; i < vertexCount - 1; i++) {
				m_SpriteVertexBuffer.push_back(vertexData[0]);
				m_SpriteVertexBuffer.push_back(vertexData[i]);
				m_SpriteVertexBuffer.push_back(vertexData[i + 1]);
			}

			break;
		}
		case DrawMode::Strip: {
			assert(vertexCount >= 3);

			for (size_t i = 0; i < vertexCount - 2; i++) {
				if (i % 2 == 0) {
					m_SpriteVertexBuffer.push_back(vertexData[i]);
					m_SpriteVertexBuffer.push_back(vertexData[i + 1]);
					m_SpriteVertexBuffer.push_back(vertexData[i + 2]);
				} else {
					m_SpriteVertexBuffer.push_back(vertexData[i + 1]);
					m_SpriteVertexBuffer.push_back(vertexData[i]);
					m_SpriteVertexBuffer.push_back(vertexData[i + 2]);
				}
			}

			break;
		}
		case DrawMode::SymmetricQuadStrip: {

			for (size_t i = 0; i < (vertexCount - 3) / 3; i++) {
				// { 1, 3, 0 } { 1, 4, 3 } { 1, 5, 4 } { 1, 2, 5 }
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 3]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 0]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 4]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 3]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 5]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 4]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 1]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 2]);
				m_SpriteVertexBuffer.push_back(vertexData[i * 3 + 5]);
			}

			break;
		}
		default:
			break;
	}

	command.VertexCountPerInstance =
	  m_SpriteVertexBuffer.size() - command.StartVertexLocation;

	m_MatrixStateBuffer.push_back(matrixState);

	DrawCommandArgument argument = {
		.renderStateIndex = (uint32_t)m_RenderStateBuffer.size() - 1
	};
	m_IndirectCommandArgumentBuffer.push_back(argument);
	m_IndirectCommandBuffer.push_back(command);
}

void
Display::CommandBatcher::InsertCompiledGeometryDrawCommand(
  DrawMode drawMode,
  MatrixState&& matrixState,
  const RageCompiledGeometry* p,
  int iMeshIndex)
{
	// TODO (^_^)

	/*assert(drawMode == DrawMode::CompiledGeometry);
	assert(m_RenderStateBuffer.size() >= 1 && "Rendering information must be set
	before drawing");

	DrawCommand command = { .useSpriteVertex = false,
							.matrixState = matrixState };

	assert(false && "TODO: fix whatever this RageCompiledGeometry thingy should
	do");

	command.renderStateIndex = m_RenderStateBuffer.size() - 1;

	m_CommandBuffer.push_back(command);*/
}

void
Display::CommandBatcher::Clear()
{
	m_IndirectCommandBuffer.clear();
	m_IndirectCommandArgumentBuffer.clear();
	m_SpriteVertexBuffer.clear();
	m_ModelVertexBuffer.clear();
	m_RenderStateBuffer.clear();
	m_MatrixStateBuffer.clear();
}
