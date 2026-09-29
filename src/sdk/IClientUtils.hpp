#pragma once

#include "types.hpp"

class IClientUtils
{
public:
	HSteamPipe getCurrentSteamPipe();
	AppId_t getAppId();
};

extern IClientUtils* g_pClientUtils;
