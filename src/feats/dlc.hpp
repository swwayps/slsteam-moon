#pragma once

#include <cstddef>
#include <cstdint>

struct AppOwnershipInfo_t;

namespace DLC
{
	bool shouldUnlockDlc(uint32_t appId);

	bool checkAppOwnership(uint32_t appId, AppOwnershipInfo_t* info);
	bool isDlcEnabled(uint32_t baseAppId, uint32_t dlcId);
	bool isAppDlcInstalled(uint32_t appId);
	bool userSubscribedInTicket(uint32_t appId);

	uint32_t getDlcCount(uint32_t appId);
	bool getDlcDataByIndex(uint32_t appId, int index, uint32_t* dlcId, bool* available, char* dlcName, size_t& dlcNameLen);
	void makeDlcAvailable(uint32_t dlcId, bool* available);
}
