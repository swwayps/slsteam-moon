#include "IClientUtils.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"
#include "steam.hpp"


HSteamPipe IClientUtils::getCurrentSteamPipe()
{
	//Offset found in IClientUtils::GetAppId
	const static auto offset = *reinterpret_cast<lm_address_t*>(Patterns::IClientUtils::Offset_GetPipeIndex.address + 0x2);
	return *reinterpret_cast<HSteamPipe*>(this + offset);
}


AppId_t IClientUtils::getAppId()
{
	return Hooks::IClientUtils_GetAppId.originalFn.fn(this);
}

IClientUtils* g_pClientUtils;
