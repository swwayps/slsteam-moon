#pragma once

#include "types.hpp"

#include <cstdint>


SDK_Class IClientUser
{
public:


	// Lua plugin API. False until the BLoggedOn VFT hook is placed.
	bool loggedOn();
	CSteamId getSteamId();
	bool setLegacyCDKey(const AppId_t appId, const char* key);
	bool updateAppOwnershipTicket(const AppId_t appId, const bool staleOnly);
};
