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

	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info, uint32_t ownerId);
	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info);

	bool checkAppOwnership(uint32_t appId, CAppOwnershipInfo* info);
	void getSubscribedApps(uint32_t* appList, uint32_t size, uint32_t& count);

	// Register the DLC appids that belong to AdditionalApps (discovered
	// from provisioned appinfo at setup).  Used by shouldDisableCDKey so
	// the launch-time legacy-key gate is suppressed for a base app AND
	// its DLC — Steam queries RequiresLegacyCDKey across the whole
	// app+DLC set, and a single DLC that still requires a key drags the
	// base app's launch into a failing GettingLegacyKey.
	void setAddedAppDlcIds(const std::vector<uint32_t>& dlcIds);
	bool isAddedAppDlcId(uint32_t appId);

	bool shouldDisableCloud(uint32_t appId);
	bool shouldDisableCDKey(uint32_t appId);
	bool shouldDisableUpdates(uint32_t appId);

	void sendGamesPlayed(CMsgClientGamesPlayed* msg);
	void sendPICSInfoRequest(CMsgClientPICSProductInfoRequest* msg);
	void sendMsg(CProtoBufMsgBase* msg);
};
