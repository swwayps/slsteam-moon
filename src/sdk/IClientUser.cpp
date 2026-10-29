#include "IClientUser.hpp"

#include "../hooks.hpp"
#include "../memhlp.hpp"
#include "../vftableinfo.hpp"


bool IClientUser::loggedOn()
{
	const auto original = Hooks::IClientUser_BLoggedOn.originalFn.fn;
	return original && original(this);
}

CSteamId IClientUser::getSteamId()
{
	const auto original = Hooks::IClientUser_GetSteamId.originalFn.fn;
	return original ? original(this) : CSteamId();
}

bool IClientUser::setLegacyCDKey(const AppId_t appId, const char* key)
{
	return MemHlp::callVFunc<bool(*)(void*, AppId_t, const char*)>(VFTIndexes::IClientUser::SetLegacyCDKey.index, this, appId, key);
}


bool IClientUser::updateAppOwnershipTicket(const AppId_t appId, const bool staleOnly)
{
	return Hooks::IClientUser_BUpdateAppOwnershipTicket.originalFn.fn(this, appId, staleOnly);
}
