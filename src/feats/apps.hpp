#pragma once

#include "../mtvar.hpp"
#include "../sdk/types.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

class CNetPacket;
class CPlayer_GetLastPlayedTimes_Response;
class CSteamId;

template<typename T> class CUtlVector;

struct AppOwnershipInfo_t;
struct AppStateInfo_t;
struct DepotInfo_t;

namespace Apps
{
	extern bool applistRequested;
	// Written by the config-store detour, read by the presence sender.
	extern MTVariable<std::unordered_set<uint32_t>> privateApps;

	bool unlockApp(uint32_t appId, AppOwnershipInfo_t* info, const CSteamId& ownerId);
	bool unlockApp(uint32_t appId, AppOwnershipInfo_t* info);

	void buildDepotDependency(uint32_t appId, CUtlVector<DepotInfo_t>* depots, CUtlVector<DepotInfo_t>* sharedDepots);
	bool checkAppOwnership(uint32_t appId, AppOwnershipInfo_t* info);
	void getAppStateInfo(uint32_t appId, AppStateInfo_t* info);
	void getLegacyCDKey(uint32_t appId);
	// The generated key for an app without a configured one: 4 * 4 A-Z/0-9.
	std::string makeLegacyCDKey(uint32_t accountId, uint32_t appId);
	void getSubscribedApps(uint32_t* appList, uint32_t size, uint32_t& count);
	void spawnGame(const GameId_t* gameId, std::string& cmd, std::string& cmdline);

	// Publish the two independent sources behind the managed-DLC membership
	// query. Appinfo refreshes replace only discoveries; config reloads replace
	// only explicit DlcData entries.
	void setDiscoveredAppDlcIds(const std::vector<uint32_t>& dlcIds);
	void setConfiguredAppDlcIds(const std::vector<uint32_t>& dlcIds);
	bool isAddedAppDlcId(uint32_t appId);

	bool shouldDisableCloud(uint32_t appId);
	bool shouldDisableUpdates(uint32_t appId);

	void sendAndRecvLastPlayedTimes(const char* name, CPlayer_GetLastPlayedTimes_Response* recv);
	void sendGamesPlayed(CNetPacket* packet);
	void sendPICSInfoRequest(CNetPacket* packet);
	void sendMsg(CNetPacket* packet);

	void setConfigStoreString(const char* key, const char* value);
};
