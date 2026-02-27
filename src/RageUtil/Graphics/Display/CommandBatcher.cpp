#include "CommandBatcher.h"
#include <cassert>
#include "CompiledGeometry.h"

void
Display::CommandBatcher::InsertRenderStateCommand(RenderState renderState)
{
	m_RenderStateBuffer.push_back(renderState);
}

void
Display::CommandBatcher::InsertPipelineChangeCommand(intptr_t pipeline,
													 bool persist)
{
	assert(!persist && "TODO: implement shader persistence (child actor shader overriding)");
	if (m_PipelineCommands.size() &&
		m_PipelineCommands.back().Pipeline == pipeline) {
		return;
	}

	m_PipelineCommands.emplace_back(pipeline, m_IndexBuffer.size());
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
	m_MatrixStateBuffer.push_back(matrixState);
	const auto previousVertexCount = m_VertexBuffer.size();
	for (int i = 0; i < vertexCount; i++) {
		m_VertexBuffer.emplace_back(vertexData[i],
									(uint32_t)m_MatrixStateBuffer.size() - 1,
									(uint32_t)m_RenderStateBuffer.size() - 1);
	}

	switch (drawMode) {
		case DrawMode::Triangles: {
			for (size_t i = 0; i < vertexCount / 3; i++) {
				m_IndexBuffer.push_back(previousVertexCount + 3 * i);
				m_IndexBuffer.push_back(previousVertexCount + 3 * i + 1);
				m_IndexBuffer.push_back(previousVertexCount + 3 * i + 2);
			}
			break;
		}
		case DrawMode::Quads: {
			for (size_t i = 0; i < vertexCount / 4; i++) {
				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 0);
				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 2);

				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 2);
				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 3);
				m_IndexBuffer.push_back(previousVertexCount + i * 4 + 0);
			}

			break;
		}
		case DrawMode::QuadStrip: {
			for (size_t i = 0; i < (vertexCount - 2) / 2; i++) {
				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 0);
				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 2);

				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 2);
				m_IndexBuffer.push_back(previousVertexCount + i * 2 + 3);
			}

			break;
		}
		case DrawMode::Fan: {
			assert(vertexCount >= 3);

			for (size_t i = 1; i < vertexCount - 1; i++) {
				m_IndexBuffer.push_back(previousVertexCount);
				m_IndexBuffer.push_back(previousVertexCount + i);
				m_IndexBuffer.push_back(previousVertexCount + i + 1);
			}

			break;
		}
		case DrawMode::Strip: {
			assert(vertexCount >= 3);

			for (size_t i = 0; i < vertexCount - 2; i++) {
				if (i % 2 == 0) {
					m_IndexBuffer.push_back(previousVertexCount + i);
					m_IndexBuffer.push_back(previousVertexCount + i + 1);
					m_IndexBuffer.push_back(previousVertexCount + i + 2);
				} else {
					m_IndexBuffer.push_back(previousVertexCount + i + 1);
					m_IndexBuffer.push_back(previousVertexCount + i);
					m_IndexBuffer.push_back(previousVertexCount + i + 2);
				}
			}

			break;
		}
		case DrawMode::SymmetricQuadStrip: {

			for (size_t i = 0; i < (vertexCount - 3) / 3; i++) {
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 3);
				m_IndexBuffer.push_back(previousVertexCount + i + 3 + 0);

				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 4);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 3);

				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 5);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 4);

				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 1);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 2);
				m_IndexBuffer.push_back(previousVertexCount + i * 3 + 5);
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
	assert(drawMode == DrawMode::CompiledGeometry);
	assert(m_RenderStateBuffer.size() >= 1 &&
		   "Rendering information must be set before drawing");

	const auto geometry = reinterpret_cast<const CompiledGeometry*>(p);
	const auto& meshInfo = geometry->m_vMeshInfo[iMeshIndex];

	m_MatrixStateBuffer.push_back(matrixState);
	if (meshInfo.m_bNeedsTextureMatrixScale) {
		m_MatrixStateBuffer.back().texture.m[3][0] = 0;
		m_MatrixStateBuffer.back().texture.m[3][1] = 0;
	}

	RageVColor whiteVColor = {};
	whiteVColor.r = UINT8_MAX;
	whiteVColor.g = UINT8_MAX;
	whiteVColor.b = UINT8_MAX;
	whiteVColor.a = UINT8_MAX;

	const auto previousVertexCount = m_VertexBuffer.size();
	for (int i = 0; i < meshInfo.iVertexCount; i++) {
		const auto& vertex = geometry->m_Vertices[meshInfo.iVertexStart + i];
		m_VertexBuffer.emplace_back(
		  RageSpriteVertex{
			.p = vertex.p, .n = vertex.n, .c = whiteVColor, .t = vertex.t },
		  (uint32_t)m_MatrixStateBuffer.size() - 1,
		  (uint32_t)m_RenderStateBuffer.size() - 1);
	}

	for (int i = meshInfo.iTriangleStart;
		 i < meshInfo.iTriangleStart + meshInfo.iTriangleCount;
		 i++) {
		for (int j = 0; j < 3; j++) {
			m_IndexBuffer.push_back(previousVertexCount +
									geometry->m_Triangles[i].nVertexIndices[j] -
									meshInfo.iVertexStart);
		}
	}
}

void
Display::CommandBatcher::Clear()
{
	m_VertexBuffer.clear();
	m_IndexBuffer.clear();
	m_RenderStateBuffer.clear();
	m_MatrixStateBuffer.clear();
	m_RenderTargetCommands.clear();
	m_PipelineCommands.clear();
}
