#pragma once

#include "libmem/libmem.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>


struct VFTableInfo_t
{
	std::string typeName;
	std::string functionName;
	lm_address_t address;
	unsigned int index;

	VFTableInfo_t(const char* typeName, const char* functionName, const unsigned int index = 0);
	bool init();

	std::string getPrintName() const;
};

namespace VFTIndexes
{
	namespace CClientUnifiedServiceTransport
	{
		extern VFTableInfo_t SendAndRecvMsg;
	}

	namespace IClientApps
	{
		extern VFTableInfo_t GetAppData;
		extern VFTableInfo_t GetAppDataSection;
		extern VFTableInfo_t RequestAppInfoUpdate;
		extern VFTableInfo_t GetDLCCount;
		extern VFTableInfo_t GetDLCDataByIndex;
		extern VFTableInfo_t GetAppType;
	}

	namespace IClientAppManager
	{
		extern VFTableInfo_t InstallApp;
		extern VFTableInfo_t UninstallApp;
		extern VFTableInfo_t LaunchApp;
		extern VFTableInfo_t GetAppInstallState;
		extern VFTableInfo_t IsAppDlcInstalled;
		extern VFTableInfo_t BIsDlcEnabled;
		extern VFTableInfo_t GetUpdateInfo;
	}

	// Called on CCompatManager, whose slots follow IClientCompatMap.
	namespace IClientCompat
	{
		extern VFTableInfo_t GetCompatToolName;
		extern VFTableInfo_t SpecifyCompatTool;
	}

	namespace IClientEngine
	{
		extern VFTableInfo_t GetClientUser;
	}

	namespace IClientRemoteStorage
	{
		extern VFTableInfo_t IsCloudEnabledForApp;
	}

	namespace IClientUtils
	{
		extern VFTableInfo_t GetOfflineMode;
		extern VFTableInfo_t GetAppId;
	}

	namespace IClientUser
	{
		extern VFTableInfo_t BLoggedOn;
		extern VFTableInfo_t BUpdateAppOwnershipTicket;
		extern VFTableInfo_t GetAppOwnershipTicketExtendedData;
		extern VFTableInfo_t GetEncryptedAppTicket;
		extern VFTableInfo_t GetSteamID;
		extern VFTableInfo_t IsUserSubscribedAppInTicket;
		extern VFTableInfo_t RequiresLegacyCDKey;
	}

	std::vector<VFTableInfo_t*>& functions();
	extern std::unordered_map<std::string, std::map<std::string, unsigned int>> tableMap;

	void dump(const std::map<std::string, unsigned int>& interfaceMap);
	bool init();
}
