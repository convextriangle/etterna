#include "LowLevelWindowVK.h"
#include "Etterna/Globals/global.h"
#include "arch/arch_default.h"
#include <cassert>

LowLevelWindowVK*
LowLevelWindowVK::Create()
{
#ifndef __unix__
	assert(false && "Should never be called");
	return nullptr;
#else
	return new LOW_LEVEL_WINDOW_VK;
#endif
}
