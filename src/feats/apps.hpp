#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

class CAppOwnershipInfo;
class CProtoBufMsgBase;
class CMsgClientGamesPlayed;
class CMsgClientPICSProductInfoRequest;

namespace Apps
{
	extern bool applistRequested;
	extern std::map<uint32_t, int> appIdOwnerOverride;

	// The generic PlayNotOwnedGames path is allowed to handle games and
	// applications, but never DLC.  DLC ownership is scoped separately through
	// isAddedAppDlcId(), so fail closed until Steam's app type is available.
	inline bool genericOwnershipOverrideAllowed(
		bool playNotOwnedGames, bool ownsLicense, bool appTypeKnown,
		bool isDlc, bool automaticFilter, bool isGameOrApplication)
	{
		if (!playNotOwnedGames || ownsLicense || !appTypeKnown || isDlc)
		{
			return false;
		}
		return !automaticFilter || isGameOrApplication;
	}

	inline bool ownershipOverrideAllowed(
		bool managedApp, bool playNotOwnedGames, bool ownsLicense,
		bool appTypeKnown, bool isDlc, bool automaticFilter,
		bool isGameOrApplication)
	{
		return managedApp || genericOwnershipOverrideAllowed(
			playNotOwnedGames, ownsLicense, appTypeKnown, isDlc,
			automaticFilter, isGameOrApplication);
	}

	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info, uint32_t ownerId);
	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info);

	bool checkAppOwnership(uint32_t appId, CAppOwnershipInfo* info);
	void getSubscribedApps(uint32_t* appList, uint32_t size, uint32_t& count);

	// Publish the two independent sources behind the managed-DLC membership
	// query. Appinfo refreshes replace only discoveries; config reloads replace
	// only explicit DlcData entries.
	void setDiscoveredAppDlcIds(const std::vector<uint32_t>& dlcIds);
	void setConfiguredAppDlcIds(const std::vector<uint32_t>& dlcIds);
	bool isAddedAppDlcId(uint32_t appId);

	bool shouldDisableCloud(uint32_t appId);
	bool shouldDisableCDKey(uint32_t appId);
	bool shouldDisableUpdates(uint32_t appId);

	void sendGamesPlayed(CMsgClientGamesPlayed* msg);
	void sendPICSInfoRequest(CMsgClientPICSProductInfoRequest* msg);
	void sendMsg(CProtoBufMsgBase* msg);
};
