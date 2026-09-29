#pragma once

#include <cstdint>
#include <map>
#include <vector>

class CNetPacket;
class CPlayer_GetLastPlayedTimes_Response;
class CSteamId;

template<typename T> class CUtlVector;

struct AppOwnershipInfo_t;
struct DepotInfo_t;

namespace Apps
{
	extern bool applistRequested;

	bool unlockApp(uint32_t appId, AppOwnershipInfo_t* info, const CSteamId& ownerId);
	bool unlockApp(uint32_t appId, AppOwnershipInfo_t* info);

	void buildDepotDependency(uint32_t appId, CUtlVector<DepotInfo_t>* depots, CUtlVector<DepotInfo_t>* sharedDepots);
	bool checkAppOwnership(uint32_t appId, AppOwnershipInfo_t* info);
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

	void sendAndRecvLastPlayedTimes(const char* name, CPlayer_GetLastPlayedTimes_Response* recv);
	void sendGamesPlayed(CNetPacket* packet);
	void sendPICSInfoRequest(CNetPacket* packet);
	void sendMsg(CNetPacket* packet);
};
