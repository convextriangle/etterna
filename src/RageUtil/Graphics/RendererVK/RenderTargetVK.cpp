#include "RenderTargetVK.h"
#include <cassert>

void
RenderTargetVK::Create(const RenderTargetParam& param,
					   int& iTextureWidthOut,
					   int& iTextureHeightOut)
{
	m_Param = param;
	const auto width = power_of_two(param.iWidth);
	const auto height = power_of_two(param.iHeight);

	iTextureWidthOut = width;
	iTextureHeightOut = height;

	m_Texture = 0;
}

auto
RenderTargetVK::GetTexture() const -> intptr_t
{
	assert(false && "Should not be called");
	return m_Texture;
}

void
RenderTargetVK::StartRenderingTo()
{
	assert(false && "Should not be called");
}

void
RenderTargetVK::FinishRenderingTo()
{
	assert(false && "Should not be called");
}
