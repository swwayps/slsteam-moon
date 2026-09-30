#pragma once

#include "types.hpp"


SDK_Class IClientUtils
{
public:
	HSteamPipe getCurrentSteamPipe();
	AppId_t getAppId();
};

extern IClientUtils* g_pClientUtils;
