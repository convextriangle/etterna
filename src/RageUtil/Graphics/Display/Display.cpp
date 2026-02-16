#include "Display.h"
#include "CompiledGeometry.h"
#include "Core/Services/Locator.hpp"
#include <cassert>
#include <source_location>

#ifdef _WIN32
#include "archutils/Win32/GraphicsWindow.h"
#else
#error Display::Display is unfinished for non-Windows platforms
#endif

Display::Display::Display(std::unique_ptr<Renderer> renderer)
  : m_Renderer(std::move(renderer))
  , m_RenderState()
{
}

std::string
Display::Display::Init(VideoModeParams&& p, bool bAllowUnacceleratedRenderer)
{
	Locator::getLogger()->info("Display::Display::Init()");
	Locator::getLogger()->info("Current renderer: UnstableDisplay - {}",
							   m_Renderer->GetApiDescription());

	GraphicsWindow::Initialize(false);

	bool ignored = false;
	return SetVideoMode(std::move(p), ignored);
}

void
Display::Display::GetDisplaySpecs(DisplaySpecs& out) const
{
}

void
Display::Display::ResolutionChanged()
{
	m_Renderer->ResolutionChanged();
	RageDisplay::ResolutionChanged();
}

bool
Display::Display::BeginFrame()
{
#ifdef _WIN32
	GraphicsWindow::Update();
#else
#error todo
#endif
	m_Batcher.Clear();
	m_RenderState.textureFiltering = true;
	m_RenderState.textureWrapping = false;

	PushCurrentRenderState();

	return m_IsInitDone && RageDisplay::BeginFrame();
}

void
Display::Display::EndFrame()
{
	m_Renderer->OnRender(GetActualVideoModeParams(), m_Batcher);
	RageDisplay::EndFrame();
}

const ActualVideoModeParams*
Display::Display::GetActualVideoModeParams() const
{
#ifdef _WIN32
	return GraphicsWindow::GetParams();
#else
#error Display::Display is unfinished for non-Windows platforms
#endif
}

std::string
Display::Display::TryVideoMode(const VideoModeParams& p, bool& bNewDeviceOut)
{
#ifdef _WIN32
	GraphicsWindow::CreateGraphicsWindow(p);
#else
#error Display::Display is unfinished for non-Windows platforms
#endif

	m_Renderer->InitializeRenderer(p);

	ResolutionChanged();

	// OnRender() with a black clearing to not whiteblast people?
	m_Renderer->OnRender(GetActualVideoModeParams(), m_Batcher);

	m_IsInitDone = true;
	return std::string();
}

#pragma region Texture handling

const RageDisplay::RagePixelFormatDesc*
Display::Display::GetPixelFormatDesc(RagePixelFormat pf) const
{
	assert(pf == RagePixelFormat_RGBA8);
	static auto desc = // silly goose
	  RagePixelFormatDesc{ 32,
						   { 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000 } };
	return &desc;
}

bool
Display::Display::SupportsTextureFormat(RagePixelFormat pixfmt, bool realtime)
{
	return pixfmt == RagePixelFormat_RGBA8;
}

intptr_t
Display::Display::CreateTexture(RagePixelFormat pixfmt,
								RageSurface* img,
								bool bGenerateMipMaps)
{
	assert(pixfmt == RagePixelFormat_RGBA8);

	return m_Renderer->CreateTexture(img);
}

void
Display::Display::UpdateTexture(intptr_t uTexHandle,
								RageSurface* img,
								int xoffset,
								int yoffset,
								int width,
								int height)
{
	m_Renderer->UpdateTexture(uTexHandle, img, xoffset, yoffset, width, height);
}

void
Display::Display::DeleteTexture(intptr_t iTexHandle)
{
	m_Renderer->DeleteTexture(iTexHandle);
}

void
Display::Display::ClearAllTextures()
{
	m_Renderer->ClearAllTextures();
}

int
Display::Display::GetNumTextureUnits()
{
	return 1;
}

int
Display::Display::GetMaxTextureSize() const
{
	return Display::Display::MaxTextureSize;
}

#pragma endregion

#pragma region RenderState handling

void
Display::Display::SetTexture(TextureUnit tu, intptr_t iTexture)
{
	assert(tu == TextureUnit_1);
	m_RenderState.textureHandle = iTexture;
}

void
Display::Display::SetTextureWrapping(TextureUnit tu, bool b)
{
	assert(tu == TextureUnit_1);
	m_RenderState.textureWrapping = b;
}

void
Display::Display::SetTextureFiltering(TextureUnit tu, bool b)
{
	assert(tu == TextureUnit_1);
	m_RenderState.textureFiltering = b;
}

#pragma endregion

#pragma region Draw queueing

void
Display::Display::DrawQuadsInternal(const RageSpriteVertex v[], int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::Quads, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawQuadStripInternal(const RageSpriteVertex v[],
										int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::QuadStrip, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawFanInternal(const RageSpriteVertex v[], int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::Fan, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawStripInternal(const RageSpriteVertex v[], int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::Strip, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawTrianglesInternal(const RageSpriteVertex v[],
										int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::Triangles, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawSymmetricQuadStripInternal(const RageSpriteVertex v[],
												 int iNumVerts)
{
	PushCurrentRenderState();

	m_Batcher.InsertSpriteDrawCommand(
	  DrawMode::SymmetricQuadStrip, GetCurrentMatrixState(), v, iNumVerts);
}

void
Display::Display::DrawCompiledGeometryInternal(const RageCompiledGeometry* p,
											   int iMeshIndex)
{
	PushCurrentRenderState();

	m_Batcher.InsertCompiledGeometryDrawCommand(
	  DrawMode::CompiledGeometry, GetCurrentMatrixState(), p, iMeshIndex);
}

#pragma endregion

#pragma region Unfinished things

intptr_t
Display::Display::CreateRenderTarget(const RenderTargetParam& param,
									 int& iTextureWidthOut,
									 int& iTextureHeightOut)
{
	assert(false && "Not implemented");
	return intptr_t();
}

intptr_t
Display::Display::GetRenderTarget()
{
	assert(false && "Not implemented");
	return intptr_t();
}

void
Display::Display::SetRenderTarget(intptr_t uTexHandle, bool bPreserveTexture)
{
	assert(false && "Not implemented");
}

#pragma endregion

RageCompiledGeometry*
Display::Display::CreateCompiledGeometry()
{
	return new CompiledGeometry;
}

void
Display::Display::DeleteCompiledGeometry(RageCompiledGeometry* p)
{
	assert(p != nullptr);
	delete p;
}

RageSurface*
Display::Display::CreateScreenshot()
{
	return m_Renderer->CreateScreenshot();
}

bool
Display::Display::SupportsThreadedRendering()
{
	return false;
}

bool
Display::Display::SupportsPerVertexMatrixScale()
{
	return false;
}

Display::MatrixState
Display::Display::GetCurrentMatrixState()
{
	MatrixState m;
	m.projection = *GetProjectionTop();
	m.view = *GetViewTop();
	m.world = *GetWorldTop();
	m.texture = *GetTextureTop();

	return m;
}

void
Display::Display::PushCurrentRenderState()
{
	if (m_RenderState == m_PreviousRenderState) {
		return;
	}

	m_PreviousRenderState = m_RenderState;
	m_Batcher.InsertRenderStateCommand(m_RenderState);
}

#pragma region Unsupported / old graphics API functions

void
Display::Display::SetBlendMode(BlendMode mode)
{
}

void
Display::Display::SetTextureMode(TextureUnit tu, TextureMode tm)
{
}

void
Display::Display::SetZWrite(bool b)
{
}

void
Display::Display::SetZBias(float f)
{
}

void
Display::Display::SetZTestMode(ZTestMode mode)
{
}

void
Display::Display::SetCullMode(CullMode mode)
{
}

void
Display::Display::SetAlphaTest(bool b)
{
}

void
Display::Display::ClearZBuffer()
{
}

bool
Display::Display::IsZWriteEnabled() const
{
	return false;
}

bool
Display::Display::IsZTestEnabled() const
{
	return false;
}

void
Display::Display::SetMaterial(const RageColor& emissive,
							  const RageColor& ambient,
							  const RageColor& diffuse,
							  const RageColor& specular,
							  float shininess)
{
}

void
Display::Display::SetLighting(bool b)
{
}

void
Display::Display::SetLightOff(int index)
{
}

void
Display::Display::SetLightDirectional(int index,
									  const RageColor& ambient,
									  const RageColor& diffuse,
									  const RageColor& specular,
									  const RageVector3& dir)
{
}

void
Display::Display::SetSphereEnvironmentMapping(TextureUnit tu, bool b)
{
}

void
Display::Display::SetCelShaded(int stage)
{
}

#pragma endregion
