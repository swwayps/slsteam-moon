#include "dlc.hpp"

#include "../sdk/CAppOwnershipInfo.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/IClientUtils.hpp"

#include "../config.hpp"

#include "apps.hpp"


bool DLC::shouldUnlockDlc(uint32_t appId)
{
	if (!g_pClientUtils || !g_pClientUtils->getAppId())
	{
		return false;
	}

	// The upstream DLC hook applied the global AppIds blacklist/whitelist to
	// every DLC queried while any game was active.  With the default empty
	// blacklist that made DLC from ordinary, genuinely-owned games eligible
	// for the local ownership override.  Scope the override to DLC ids
	// discovered from LuaTools-managed base apps or explicitly declared under
	// a managed parent's DlcData instead.  This deliberately
	// does not depend on ownership of the base app: adding an already-owned
	// game through LuaTools must still enable its managed DLC set.
	if (!Apps::isAddedAppDlcId(appId))
	{
		return false;
	}

	if (g_config.shouldExcludeAppId(appId))
	{
		return false;
	}

	CUser* user = getLocalUser();
	if (user != nullptr && user->isSubscribed(appId))
	{
		return false;
	}
	
	return true;
}

bool DLC::checkAppOwnership(uint32_t appId, CAppOwnershipInfo *info)
{
	if (!shouldUnlockDlc(appId))
	{
		return false;
	}

	Apps::unlockApp(appId, info);

	return true;
}

bool DLC::isDlcEnabled(uint32_t baseAppId, uint32_t dlcId)
{
	(void)baseAppId;
	return shouldUnlockDlc(dlcId);
}

bool DLC::isAppDlcInstalled(uint32_t appId)
{
	return shouldUnlockDlc(appId);
}

bool DLC::userSubscribedInTicket(uint32_t appId)
{
	//Might want to compare the steamId param to the g_currentSteamId in the future
	//Although not doing that might also work for Dedicated servers?
	return shouldUnlockDlc(appId);
}

uint32_t DLC::getDlcCount(uint32_t appId)
{
	if (!g_config.managedAppIds.get().contains(appId))
	{
		return 0;
	}

	const auto dlcData = g_config.dlcData.get();
	if (dlcData.contains(appId))
	{
		return dlcData.at(appId).dlcIds.size();
	}

	return 0;
}

bool DLC::getDlcDataByIndex(uint32_t appId, int index, uint32_t* dlcId, bool* available, char* dlcName, size_t& dlcNameLen)
{
	if (!dlcId || !available || !dlcName
	    || !g_config.managedAppIds.get().contains(appId))
	{
		return false;
	}

	auto dlcData = g_config.dlcData.get();
	if (dlcData.contains(appId))
	{
		auto& data = dlcData[appId];
		auto dlc = std::next(data.dlcIds.begin(), index);

		*dlcId = dlc->first;
		*available = Apps::isAddedAppDlcId(*dlcId) &&
			!g_config.shouldExcludeAppId(*dlcId);

		//No clue if we have to check for errors during printf since the devs hopefully didn't fuck
		//up the dlcNameLen. Who knows though
		snprintf(dlcName, dlcNameLen, "%s", dlc->second.c_str());

		return true;
	}
	return false;
}

void DLC::makeDlcAvailable(uint32_t dlcId, bool* available)
{
	if (available && Apps::isAddedAppDlcId(dlcId)
	    && !g_config.shouldExcludeAppId(dlcId))
	{
		*available = true;
	}
}
