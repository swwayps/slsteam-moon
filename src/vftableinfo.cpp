#include "vftableinfo.hpp"

#include "decompiler.hpp"
#include "log.hpp"

#include <sstream>


VFTableInfo_t::VFTableInfo_t(const char* typeName, const char* functionName, const unsigned int index)
	:
	typeName(typeName), functionName(functionName), index(index)
{
	VFTIndexes::functions().emplace_back(this);
}

bool VFTableInfo_t::init()
{
	if (!Decompiler::vftables.contains(typeName))
	{
		g_pLog->debug("%s not found in Decompiler::vftables!\n", typeName);
		return false;
	}

	if (!index)
	{
		if (!VFTIndexes::tableMap.contains(typeName))
		{
			VFTIndexes::tableMap[typeName] = Decompiler::parseInterfaceMapBase(typeName.c_str());
		}

		const auto& tbl = VFTIndexes::tableMap.at(typeName);
		if (!tbl.contains(functionName))
		{
			g_pLog->debug("VFunction %s not found!\n", functionName.c_str());
			return false;
		}

		index = tbl.at(functionName);
	}

	auto& vft = Decompiler::vftables[typeName];
	auto& funcs = vft.functions;
	vft.analzye();

	if (index >= funcs.size())
	{
		g_pLog->debug("%s index bigger than vtable size!\n", getPrintName().c_str());
		return false;
	}

	address = funcs.at(index);
	g_pLog->debug("%s at index %u\n", getPrintName().c_str(), index);
	return true;
}

std::string VFTableInfo_t::getPrintName() const
{
	return typeName + "::" + functionName;
}


namespace VFTIndexes
{
	namespace CClientUnifiedServiceTransport
	{
		VFTableInfo_t SendAndRecvMsg
		{
			"30CClientUnifiedServiceTransport",
			"SendAndRecv",
			5
		};
	}

	namespace IClientApps
	{
		VFTableInfo_t GetAppData
		{
			"14IClientAppsMap",
			"GetAppData"
		};
		VFTableInfo_t GetAppDataSection
		{
			"14IClientAppsMap",
			"GetAppDataSection"
		};
		VFTableInfo_t RequestAppInfoUpdate
		{
			"14IClientAppsMap",
			"RequestAppInfoUpdate"
		};
		VFTableInfo_t GetDLCCount
		{
			"14IClientAppsMap",
			"GetDLCCount"
		};
		VFTableInfo_t GetDLCDataByIndex
		{
			"14IClientAppsMap",
			"BGetDLCDataByIndex"
		};
		VFTableInfo_t GetAppType
		{
			"14IClientAppsMap",
			"GetAppType"
		};
	}

	namespace IClientAppManager
	{
		VFTableInfo_t InstallApp
		{
			"20IClientAppManagerMap",
			"InstallApp"
		};
		VFTableInfo_t UninstallApp
		{
			"20IClientAppManagerMap",
			"UninstallApp"
		};
		VFTableInfo_t LaunchApp
		{
			"20IClientAppManagerMap",
			"LaunchApp"
		};
		VFTableInfo_t GetAppInstallState
		{
			"20IClientAppManagerMap",
			"GetAppInstallState"
		};
		VFTableInfo_t IsAppDlcInstalled
		{
			"20IClientAppManagerMap",
			"IsAppDlcInstalled"
		};
		VFTableInfo_t BIsDlcEnabled
		{
			"20IClientAppManagerMap",
			"BIsDlcEnabled"
		};
		VFTableInfo_t GetUpdateInfo
		{
			"20IClientAppManagerMap",
			"GetUpdateInfo"
		};
	}

	//namespace IClientEngine
	//{
	//	VFTableInfo_t GetClientUser
	//	{
	//		//"13IClientEngine",
	//		"12CSteamClient",
	//		"GetClientUser",
	//		7
	//	};
	//}

	namespace IClientCompat
	{
		VFTableInfo_t GetCompatToolName
		{
			"16IClientCompatMap",
			"GetCompatToolName"
		};
		VFTableInfo_t SpecifyCompatTool
		{
			"16IClientCompatMap",
			"SpecifyCompatTool"
		};
	}

	namespace IClientRemoteStorage
	{
		VFTableInfo_t IsCloudEnabledForApp
		{
			"23IClientRemoteStorageMap",
			"IsCloudEnabledForApp"
		};
	}

	namespace IClientUtils
	{
		VFTableInfo_t GetOfflineMode
		{
			"15IClientUtilsMap",
			"GetOfflineMode"
		};
		VFTableInfo_t GetAppId
		{
			"15IClientUtilsMap",
			"GetAppID"
		};
	}

	namespace IClientUser
	{
		VFTableInfo_t BLoggedOn
		{
			"14IClientUserMap",
			"BLoggedOn"
		};
		VFTableInfo_t BUpdateAppOwnershipTicket
		{
			"14IClientUserMap",
			"BUpdateAppOwnershipTicket"
		};
		VFTableInfo_t GetAppOwnershipTicketExtendedData
		{
			"14IClientUserMap",
			"GetAppOwnershipTicketExtendedData"
		};
		VFTableInfo_t GetEncryptedAppTicket
		{
			"14IClientUserMap",
			"GetEncryptedAppTicket"
		};
		VFTableInfo_t GetSteamID
		{
			"14IClientUserMap",
			"GetSteamID"
		};
		VFTableInfo_t IsUserSubscribedAppInTicket
		{
			"14IClientUserMap",
			"IsUserSubscribedAppInTicket"
		};
		VFTableInfo_t RequiresLegacyCDKey
		{
			"14IClientUserMap",
			"RequiresLegacyCDKey"
		};
	}

	std::vector<VFTableInfo_t*>& functions()
	{
		// Function-local static like Patterns::patterns(): under GCC 11 (the
		// release builder) a namespace-scope vector is dynamically initialized
		// after the entries above and drops their registrations.
		static std::vector<VFTableInfo_t*> instance;
		return instance;
	}
	std::unordered_map<std::string, std::map<std::string, unsigned int>> tableMap;
}

void VFTIndexes::dump(const std::map<std::string, unsigned int>& interfaceMap)
{
	std::ostringstream ss;

	for(const auto& kv : interfaceMap)
	{
		ss << "\n" << kv.first << " = " << kv.second;
	}

	g_pLog->debug("Dump %s\n", ss.str().c_str());
}

bool VFTIndexes::init()
{
	for(const auto& fn : functions())
	{
		if (!fn->init())
		{
			return false;
		}
	}

	return true;
}
