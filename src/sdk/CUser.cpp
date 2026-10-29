#include "CUser.hpp"

#include "CUser.hpp"
#include "IClientAppManager.hpp"
#include "IClientApps.hpp"
#include "IClientUser.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"


// The member accessors add byte offsets to this, which holds while CUser
// stays an opaque (empty) handle.
static_assert(sizeof(CUser) == 1);

IClientAppManager* CUser::getAppManager()
{
	const static lm_address_t offset = MemHlp::searchOffsetByTypeName(this, "15CUserAppManager");
	return offset == LM_ADDRESS_BAD ? nullptr : reinterpret_cast<IClientAppManager*>(this + offset);
}

IClientApps* CUser::getClientApps()
{
	const static lm_address_t offset = MemHlp::searchOffsetByTypeName(this, "12CUserAppInfo");
	return offset == LM_ADDRESS_BAD ? nullptr : reinterpret_cast<IClientApps*>(this + offset);
}

IClientUser* CUser::getClientUser()
{
	const static lm_address_t offset = MemHlp::searchOffsetByTypeName(this, "5CUser");
	return offset == LM_ADDRESS_BAD ? nullptr : reinterpret_cast<IClientUser*>(this + offset);
}

bool CUser::checkAppOwnership(const AppId_t appId, AppOwnershipInfo_t* pInfo)
{
	const auto original = Hooks::CUser_CheckAppOwnership.tramp.fn;
	return original && original(this, appId, pInfo);
}

bool CUser::isSubscribed(const AppId_t appId)
{
	AppOwnershipInfo_t info {};
	if (!checkAppOwnership(appId, &info))
	{
		return false;
	}

	return info.ownsLicense && !info.licenseExpired;
}

void CUser::postCallback(const ECallbackType type, void* pCallback, const uint32_t callbackSize)
{
	const static auto fn = reinterpret_cast<void(*)(void*, ECallbackType, void*, uint32_t, uint32_t)>(Patterns::CUser::PostCallback.address);
	fn(this, type, pCallback, callbackSize, 0);
}

void CUser::updateAppOwnershipTicket(const AppId_t appId, void* pTicket, const uint32_t len)
{
	const static auto fn = reinterpret_cast<void(*)(void*, uint32_t, void*, uint32_t)>(Patterns::CUser::UpdateAppOwnershipTicket.address);
	fn(this, appId, pTicket, len);

	//Dunno if this achieves anything, but the client does it so we do too
	AppOwnershipTicketReceived_t cb;
	cb.result = k_EResultOK;
	cb.appId = appId;
	postCallback(ECallbackType::AppOwnershipTicketReceived_t, &cb, sizeof(cb));
}

bool CUser::notifyLicensesUpdated()
{
	const auto addr = Patterns::CUser::NotifyLicensesUpdated.address;
	if (addr == LM_ADDRESS_BAD)
	{
		// Pattern didn't resolve on this build — degrade to a safe
		// no-op so we don't regress the warm-cache path.
		return false;
	}

	// cdecl, single arg (`this`).  The function rebuilds the
	// LicensesUpdated_t callback from this user's own license vector
	// and posts it to every subscriber.
	const static auto fn = reinterpret_cast<void(*)(void*)>(addr);
	fn(this);
	return true;
}
