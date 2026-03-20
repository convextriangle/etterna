#include "LowLevelWindowVK.h"
#include "Etterna/Globals/global.h"
#include "arch/arch_default.h"

LowLevelWindowVK*
LowLevelWindowVK::Create()
{
	return new LOW_LEVEL_WINDOW_VK;
}
