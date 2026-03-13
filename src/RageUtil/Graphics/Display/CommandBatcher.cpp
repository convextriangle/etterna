#include "CommandBatcher.h"
#include <cassert>
#include "CompiledGeometry.h"

void
Display::CommandBatcher::InsertPipelineChangeCommand(intptr_t pipeline,
													 intptr_t vertexShaderInfo,
													 intptr_t fragShaderInfo,
													 bool persist)
{
	PipelineSettings settings = {};
	if (persist) {
		if (pipeline) {
			m_PipelineStack.emplace(pipeline, vertexShaderInfo, fragShaderInfo);
		} else {
			m_PipelineStack.pop();
		}
		settings = m_PipelineStack.top();
	} else {
		if (m_PipelineStack.size()) {
			return;
		}
		settings = { pipeline, vertexShaderInfo, fragShaderInfo };
	}

	if (!m_RenderNodes.size()) {
		m_RenderNodes.emplace_back();
	}

	m_RenderNodes.back().DrawCalls.emplace_back(
	  settings, m_IndexBuffer.size(), 0);

	m_CurrentPipeline = settings;
}

void
Display::CommandBatcher::InsertRenderTargetCommand(intptr_t renderTarget,
												   bool preserveTexture)
{
	m_RenderNodes.emplace_back(renderTarget,
							   preserveTexture,
							   std::vector<DrawCall>(),
							   std::set<size_t>());
	m_RenderTargetLookup.insert({ renderTarget, m_RenderNodes.size() - 1 });
}

uint32_t
GetSamplerFlagsFromRenderState(const Display::RenderState& state)
{
	return (uint8_t)state.textureWrapping |
		   ((uint8_t)state.textureFiltering << 1);
}

void
Display::CommandBatcher::InsertSpriteDrawCommand(
  DrawMode drawMode,
  MatrixState&& matrixState,
  const RageSpriteVertex* vertexData,
  int vertexCount,
  const RenderState& renderState)
{
	assert(drawMode != DrawMode::Invalid);
	assert(drawMode != DrawMode::CompiledGeometry);

	// -- changing draw mode in the middle of the queue would likely require
	// switching pipeline state objects
	//	  (pipeline objects are chonky)
	//	  and most of the ye olde draw modes aren't supported
	//    so just convert to a triangle list
	// -- unrolled loops look funny though
	m_MatrixStateBuffer.push_back(matrixState);
	m_DrawSettingsBuffer.emplace_back(
	  m_VertexBuffer.size(),
	  (uint32_t)m_MatrixStateBuffer.size() - 1,
	  (uint32_t)renderState.textureHandle,
	  GetSamplerFlagsFromRenderState(renderState));

	const auto previousVertexCount = m_VertexBuffer.size();
	m_VertexBuffer.resize(previousVertexCount + vertexCount);
	std::memcpy(&m_VertexBuffer[previousVertexCount],
				vertexData,
				sizeof(RageSpriteVertex) * vertexCount);

	const auto prevCount = m_IndexBuffer.size();
	switch (drawMode) {
		case DrawMode::Triangles: {
			m_IndexBuffer.resize(prevCount + vertexCount);
			for (size_t i = 0; i < vertexCount / 3; i++) {
				m_IndexBuffer[prevCount + 3 * i] = previousVertexCount + 3 * i;
				m_IndexBuffer[prevCount + 3 * i + 1] =
				  previousVertexCount + 3 * i + 1;
				m_IndexBuffer[prevCount + 3 * i + 2] =
				  previousVertexCount + 3 * i + 2;
			}
			break;
		}
		case DrawMode::Quads: {
			m_IndexBuffer.resize(prevCount + 6 * vertexCount / 4);
			for (size_t i = 0; i < vertexCount / 4; i++) {
				m_IndexBuffer[prevCount + i * 6 + 0] =
				  previousVertexCount + i * 4 + 0;
				m_IndexBuffer[prevCount + i * 6 + 1] =
				  previousVertexCount + i * 4 + 1;
				m_IndexBuffer[prevCount + i * 6 + 2] =
				  previousVertexCount + i * 4 + 2;
				m_IndexBuffer[prevCount + i * 6 + 3] =
				  previousVertexCount + i * 4 + 2;
				m_IndexBuffer[prevCount + i * 6 + 4] =
				  previousVertexCount + i * 4 + 3;
				m_IndexBuffer[prevCount + i * 6 + 5] =
				  previousVertexCount + i * 4 + 0;
			}

			break;
		}
		case DrawMode::QuadStrip: {
			m_IndexBuffer.resize(prevCount + 6 * (vertexCount - 2) / 2);
			for (size_t i = 0; i < (vertexCount - 2) / 2; i++) {
				m_IndexBuffer[prevCount + i * 6 + 0] =
				  previousVertexCount + i * 2 + 0;
				m_IndexBuffer[prevCount + i * 6 + 1] =
				  previousVertexCount + i * 2 + 1;
				m_IndexBuffer[prevCount + i * 6 + 2] =
				  previousVertexCount + i * 2 + 2;
				m_IndexBuffer[prevCount + i * 6 + 3] =
				  previousVertexCount + i * 2 + 1;
				m_IndexBuffer[prevCount + i * 6 + 4] =
				  previousVertexCount + i * 2 + 2;
				m_IndexBuffer[prevCount + i * 6 + 5] =
				  previousVertexCount + i * 2 + 3;
			}

			break;
		}
		case DrawMode::Fan: {
			assert(vertexCount >= 3);
			m_IndexBuffer.resize(prevCount + 3 * (vertexCount - 2));
			for (size_t i = 1; i < vertexCount - 1; i++) {
				m_IndexBuffer[prevCount + 3 * i] = previousVertexCount;
				m_IndexBuffer[prevCount + 3 * i + 1] = previousVertexCount + i;
				m_IndexBuffer[prevCount + 3 * i + 2] =
				  previousVertexCount + i + 1;
			}

			break;
		}
		case DrawMode::Strip: {
			assert(vertexCount >= 3);
			m_IndexBuffer.resize(prevCount + 3 * (vertexCount - 2));

			for (size_t i = 0; i < vertexCount - 2; i++) {
				if (i % 2 == 0) {
					m_IndexBuffer[prevCount + 3 * i] = previousVertexCount + i;
					m_IndexBuffer[prevCount + 3 * i + 1] =
					  previousVertexCount + i + 1;
					m_IndexBuffer[prevCount + 3 * i + 2] =
					  previousVertexCount + i + 2;
				} else {
					m_IndexBuffer[prevCount + 3 * i] =
					  previousVertexCount + i + 1;
					m_IndexBuffer[prevCount + 3 * i + 1] =
					  previousVertexCount + i;
					m_IndexBuffer[prevCount + 3 * i + 2] =
					  previousVertexCount + i + 2;
				}
			}

			break;
		}
		case DrawMode::SymmetricQuadStrip: {
			m_IndexBuffer.resize(prevCount + 12 * (vertexCount - 3) / 3);
			for (size_t i = 0; i < (vertexCount - 3) / 3; i++) {
				m_IndexBuffer[prevCount + i * 12 + 0] =
				  previousVertexCount + i * 3 + 1;
				m_IndexBuffer[prevCount + i * 12 + 1] =
				  previousVertexCount + i * 3 + 3;
				m_IndexBuffer[prevCount + i * 12 + 2] =
				  previousVertexCount + i * 3 + 0;
				m_IndexBuffer[prevCount + i * 12 + 3] =
				  previousVertexCount + i * 3 + 1;
				m_IndexBuffer[prevCount + i * 12 + 4] =
				  previousVertexCount + i * 3 + 4;
				m_IndexBuffer[prevCount + i * 12 + 5] =
				  previousVertexCount + i * 3 + 3;
				m_IndexBuffer[prevCount + i * 12 + 6] =
				  previousVertexCount + i * 3 + 1;
				m_IndexBuffer[prevCount + i * 12 + 7] =
				  previousVertexCount + i * 3 + 5;
				m_IndexBuffer[prevCount + i * 12 + 8] =
				  previousVertexCount + i * 3 + 4;
				m_IndexBuffer[prevCount + i * 12 + 9] =
				  previousVertexCount + i * 3 + 1;
				m_IndexBuffer[prevCount + i * 12 + 10] =
				  previousVertexCount + i * 3 + 2;
				m_IndexBuffer[prevCount + i * 12 + 11] =
				  previousVertexCount + i * 3 + 5;
			}

			break;
		}
		default:
			throw std::runtime_error("Unknown draw command type");
	}

	HandleDrawCommand(prevCount, m_IndexBuffer.size() - prevCount, renderState);
}

void
Display::CommandBatcher::InsertCompiledGeometryDrawCommand(
  MatrixState&& matrixState,
  const RageCompiledGeometry* p,
  int iMeshIndex,
  const RenderState& renderState)
{
	const auto geometry = reinterpret_cast<const CompiledGeometry*>(p);
	const auto& meshInfo = geometry->m_vMeshInfo[iMeshIndex];

	m_MatrixStateBuffer.push_back(matrixState);
	if (meshInfo.m_bNeedsTextureMatrixScale) {
		m_MatrixStateBuffer.back().texture.m[3][0] = 0;
		m_MatrixStateBuffer.back().texture.m[3][1] = 0;
	}

	m_DrawSettingsBuffer.emplace_back(
	  m_VertexBuffer.size(),
	  (uint32_t)m_MatrixStateBuffer.size() - 1,
	  (uint32_t)renderState.textureHandle,
	  GetSamplerFlagsFromRenderState(renderState));

	RageVColor whiteVColor = {};
	whiteVColor.r = UINT8_MAX;
	whiteVColor.g = UINT8_MAX;
	whiteVColor.b = UINT8_MAX;
	whiteVColor.a = UINT8_MAX;

	const auto previousVertexCount = m_VertexBuffer.size();
	for (int i = 0; i < meshInfo.iVertexCount; i++) {
		const auto& vertex = geometry->m_Vertices[meshInfo.iVertexStart + i];
		m_VertexBuffer.emplace_back(vertex.p, vertex.n, whiteVColor, vertex.t);
	}

	const auto prevIndexCount = m_IndexBuffer.size();

	for (int i = meshInfo.iTriangleStart;
		 i < meshInfo.iTriangleStart + meshInfo.iTriangleCount;
		 i++) {
		for (int j = 0; j < 3; j++) {
			m_IndexBuffer.push_back(previousVertexCount +
									geometry->m_Triangles[i].nVertexIndices[j] -
									meshInfo.iVertexStart);
		}
	}

	HandleDrawCommand(
	  prevIndexCount, m_IndexBuffer.size() - prevIndexCount, renderState);
}

void
Display::CommandBatcher::HandleDrawCommand(int indexOffset,
										   int indexCount,
										   const RenderState& renderState)
{
	assert(m_RenderNodes.size());
	assert(m_RenderNodes.back().DrawCalls.size());

	auto& call = m_RenderNodes.back().DrawCalls.back();

	// if we previously filled in a different draw call, we should create a new
	// one
	if (call.IndexCount != 0 && call.IndexOffset != 0 &&
		call.IndexCount + call.IndexOffset != indexOffset) {
		m_RenderNodes.back().DrawCalls.emplace_back(
		  m_CurrentPipeline, indexOffset, 0);
		call = m_RenderNodes.back().DrawCalls.back();
	}

	call.IndexOffset += indexCount;

	auto rtNodes = m_RenderTargetLookup.equal_range(renderState.textureHandle);
	for (auto i = rtNodes.first; i != rtNodes.second; i++) {
		m_RenderNodes.back().Dependencies.push_back(i->second);
	}
}

void
Display::CommandBatcher::Clear()
{
	m_VertexBuffer.clear();
	m_DrawSettingsBuffer.clear();
	m_IndexBuffer.clear();
	m_MatrixStateBuffer.clear();
	m_RenderNodes.clear();
	m_RenderTargetLookup.clear();

	// std::stack has no .clear() :|
	while (m_PipelineStack.size()) {
		m_PipelineStack.pop();
	}

	m_CurrentPipeline = {};

	m_SortedNodes.clear();
	m_VisitedNodes.clear();
}

void
Display::CommandBatcher::SortRenderNodes()
{
	m_VisitedNodes.resize(m_RenderNodes.size());
	for (size_t i = 0; i < m_RenderNodes.size(); i++) {
		if (!m_VisitedNodes[i]) {
			RenderNodeSearch(i);
		}
	}

	std::reverse(m_SortedNodes.begin(), m_SortedNodes.end());
}

void
Display::CommandBatcher::RenderNodeSearch(size_t nodeIndex)
{
	m_VisitedNodes[nodeIndex] = true;
	for (auto& next : m_RenderNodes[nodeIndex].Dependencies) {
		if (!m_VisitedNodes[next]) {
			RenderNodeSearch(nodeIndex);
		}
	}

	m_SortedNodes.push_back(nodeIndex);
}
