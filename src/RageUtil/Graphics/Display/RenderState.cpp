#include "RenderState.h"
#include <algorithm>
#include <ranges>

bool
Display::RenderState::operator==(RenderState& rhs)
{
	return std::tie(textureWrapping, textureFiltering, textureHandle) ==
		   std::tie(
			 rhs.textureWrapping, rhs.textureFiltering, rhs.textureHandle);
}
