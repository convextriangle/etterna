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
	m_MatrixStateBuffer.push_back(matrixState);

	Triangle triangle = { {},
						  (uint32_t)m_MatrixStateBuffer.size() - 1,
						  (uint32_t)m_RenderStateBuffer.size() - 1 };

	switch (drawMode) {
		case DrawMode::Triangles: {
			for (size_t i = 0; i < vertexCount / 3; i++) {
				triangle.Vertex[0] = vertexData[3 * i];
				triangle.Vertex[1] = vertexData[3 * i + 1];
				triangle.Vertex[2] = vertexData[3 * i + 2];
				m_TriangleBuffer.push_back(triangle);
			}
			break;
		}
		case DrawMode::Quads: {
			for (size_t i = 0; i < vertexCount / 4; i++) {
				triangle.Vertex[0] = vertexData[i * 4 + 0];
				triangle.Vertex[1] = vertexData[i * 4 + 1];
				triangle.Vertex[2] = vertexData[i * 4 + 2];
				m_TriangleBuffer.push_back(triangle);

				triangle.Vertex[0] = vertexData[i * 4 + 2];
				triangle.Vertex[1] = vertexData[i * 4 + 3];
				triangle.Vertex[2] = vertexData[i * 4 + 0];
				m_TriangleBuffer.push_back(triangle);
			}

			break;
		}
		case DrawMode::QuadStrip: {
			for (size_t i = 0; i < (vertexCount - 2) / 2; i++) {
				triangle.Vertex[0] = vertexData[i * 2 + 0];
				triangle.Vertex[1] = vertexData[i * 2 + 1];
				triangle.Vertex[2] = vertexData[i * 2 + 2];
				m_TriangleBuffer.push_back(triangle);

				triangle.Vertex[0] = vertexData[i * 2 + 1];
				triangle.Vertex[1] = vertexData[i * 2 + 2];
				triangle.Vertex[2] = vertexData[i * 2 + 3];
				m_TriangleBuffer.push_back(triangle);
			}

			break;
		}
		case DrawMode::Fan: {
			assert(vertexCount >= 3);

			for (size_t i = 1; i < vertexCount - 1; i++) {
				triangle.Vertex[0] = vertexData[0];
				triangle.Vertex[1] = vertexData[i];
				triangle.Vertex[2] = vertexData[i + 1];
				m_TriangleBuffer.push_back(triangle);
			}

			break;
		}
		case DrawMode::Strip: {
			assert(vertexCount >= 3);

			for (size_t i = 0; i < vertexCount - 2; i++) {
				if (i % 2 == 0) {
					triangle.Vertex[0] = vertexData[i];
					triangle.Vertex[1] = vertexData[i + 1];
					triangle.Vertex[2] = vertexData[i + 2];
					m_TriangleBuffer.push_back(triangle);
				} else {
					triangle.Vertex[0] = vertexData[i + 1];
					triangle.Vertex[1] = vertexData[i];
					triangle.Vertex[2] = vertexData[i + 2];
					m_TriangleBuffer.push_back(triangle);
				}
			}

			break;
		}
		case DrawMode::SymmetricQuadStrip: {

			for (size_t i = 0; i < (vertexCount - 3) / 3; i++) {
				triangle.Vertex[0] = vertexData[i * 3 + 1];
				triangle.Vertex[1] = vertexData[i * 3 + 3];
				triangle.Vertex[2] = vertexData[i + 3 + 0];
				m_TriangleBuffer.push_back(triangle);

				triangle.Vertex[0] = vertexData[i * 3 + 1];
				triangle.Vertex[1] = vertexData[i * 3 + 4];
				triangle.Vertex[2] = vertexData[i * 3 + 3];
				m_TriangleBuffer.push_back(triangle);

				triangle.Vertex[0] = vertexData[i * 3 + 1];
				triangle.Vertex[1] = vertexData[i * 3 + 5];
				triangle.Vertex[2] = vertexData[i * 3 + 4];
				m_TriangleBuffer.push_back(triangle);

				triangle.Vertex[0] = vertexData[i * 3 + 1];
				triangle.Vertex[1] = vertexData[i * 3 + 2];
				triangle.Vertex[2] = vertexData[i * 3 + 5];
				m_TriangleBuffer.push_back(triangle);
			}

			break;
		}
		default:
			break;
	}
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
	m_TriangleBuffer.clear();
	m_RenderStateBuffer.clear();
	m_MatrixStateBuffer.clear();
}
