#pragma once

#include <cstdint>
#include <map>
#include <vector>

class CAppOwnershipInfo;
class CNetPacket;
class CSteamId;

namespace Apps
{
	extern bool applistRequested;

	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info, const CSteamId& ownerId);
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

	void sendGamesPlayed(CNetPacket* packet);
	void sendPICSInfoRequest(CNetPacket* packet);
	void sendMsg(CNetPacket* packet);
};
