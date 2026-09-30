#include "IClientUser.hpp"

#include "../hooks.hpp"
#include "../memhlp.hpp"
#include "../vftableinfo.hpp"


bool IClientUser::loggedOn()
{
	const auto original = Hooks::IClientUser_BLoggedOn.originalFn.fn;
	return original && original(this);
}

bool IClientUser::setLegacyCDKey(const AppId_t appId, const char* key)
{
	return MemHlp::callVFunc<bool(*)(void*, AppId_t, const char*)>(VFTIndexes::IClientUser::SetLegacyCDKey.index, this, appId, key);
}
