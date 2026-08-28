#include "CUser.hpp"

#include "CAppOwnershipInfo.hpp"
#include "EResult.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"


bool CUser::checkAppOwnership(uint32_t appId, CAppOwnershipInfo* pInfo)
{
	const auto original = Hooks::CUser_CheckAppOwnership.tramp.fn;
	return original && original(this, appId, pInfo);
}

bool CUser::isSubscribed(uint32_t appId)
{
	CAppOwnershipInfo info {};
	if (!checkAppOwnership(appId, &info))
	{
		return false;
	}

	return info.ownsLicense && !info.licenseExpired;
}

void CUser::postCallback(ECallbackType type, void* pCallback, uint32_t callbackSize)
{
	const static auto fn = reinterpret_cast<void(*)(void*, ECallbackType, void*, uint32_t, uint32_t)>(Patterns::CUser::PostCallback.address);
	fn(this, type, pCallback, callbackSize, 0);
}

void CUser::updateAppOwnershipTicket(uint32_t appId, void* pTicket, uint32_t len)
{
	const static auto fn = reinterpret_cast<void(*)(void*, uint32_t, void*, uint32_t)>(Patterns::CUser::UpdateAppOwnershipTicket.address);
	fn(this, appId, pTicket, len);

	//Dunno if this achieves anything, but the client does it so we do too
	AppOwnershipTicketReceived_t cb;
	cb.result = ERESULT_OK;
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
