#include "vftableinfo.hpp"

#include "config.hpp"
#include "decompiler.hpp"
#include "log.hpp"

#include <sstream>
#include <unordered_map>

namespace
{
// The anchor slot's function references the expected string (PIC-relative
// string loads found by the decompiler's function walk).
bool slotReferences(const std::vector<lm_address_t>& funcs, const VFTableInfo_t::Anchor& anchor)
{
	if (anchor.index >= funcs.size()) return false;
	auto refs = std::unordered_map<lm_address_t, unsigned int>();
	Decompiler::parseFunction(funcs[anchor.index], refs);
	for (const auto& ref : refs)
	{
		const auto it = Decompiler::strings.find(ref.first);
		if (it != Decompiler::strings.end() &&
		    it->second.find(anchor.string) != std::string::npos)
			return true;
	}
	return false;
}
}

VFTableInfo_t::VFTableInfo_t
(
	const char* typeName,
	const char* functionName,
	const unsigned int index,
	const unsigned int subClassIndex
)
	:
	typeName(typeName),
	functionName(functionName),
	index(index),
	subClassIndex(subClassIndex)
{
	VFTIndexes::functions().emplace_back(this);
}

VFTableInfo_t::VFTableInfo_t
(
	const char* typeName,
	const char* functionName,
	unsigned int index,
	Anchor anchor,
	bool optional
)
	:
	typeName(typeName),
	functionName(functionName),
	index(index),
	subClassIndex(NO_INDEX),
	anchor(anchor),
	optional(optional)
{
	VFTIndexes::functions().emplace_back(this);
}

bool VFTableInfo_t::init()
{
	const auto fail = [this]
	{
		address = 0;
		return optional;
	};

	if (!Decompiler::vftables.contains(typeName))
	{
		LOG_ERROR("%s not found in Decompiler::vftables!\n", typeName.c_str());
		return fail();
	}

	if (index == NO_INDEX)
	{
		if (!VFTIndexes::tableMap.contains(typeName))
		{
			VFTIndexes::tableMap[typeName] = Decompiler::parseInterfaceMapBase(typeName.c_str());
		}

		const auto& tbl = VFTIndexes::tableMap.at(typeName);
		if (!tbl.contains(functionName))
		{
			LOG_ERROR("%s not found!\n", functionName.c_str());
			return fail();
		}

		index = tbl.at(functionName);
	}

	//Create copy
	auto vft = Decompiler::vftables[typeName];
	if (subClassIndex != NO_INDEX)
	{
		if (subClassIndex >= vft.subclasses.size())
		{
			LOG_ERROR("SubClassIndex %u is out of range for %s!\n", subClassIndex, getPrintName().c_str());
			return fail();
		}

		vft = vft.subclasses[subClassIndex];
	}

	auto& funcs = vft.functions;

	if (index >= funcs.size())
	{
		LOG_ERROR("%s index bigger than vtable size!\n", getPrintName().c_str());
		return fail();
	}

	if (anchor.string != nullptr && !slotReferences(funcs, anchor))
	{
		LOG_ERROR("%s: slot %u no longer references \"%s\"; the vtable layout changed%s\n",
		          getPrintName().c_str(), anchor.index, anchor.string,
		          optional ? ", feature disabled" : "");
		return fail();
	}

	address = funcs.at(index);
	LOG_DEBUG("%s at index %u, address 0x%lx\n", getPrintName().c_str(), index, address);
	return true;
}

std::string VFTableInfo_t::getPrintName() const
{
	std::ostringstream ss;
	ss << typeName << "::" << functionName;

	return ss.str();
}

namespace VFTIndexes
{
	namespace CCMInterface
	{
		VFTableInfo_t RecvPkt
		{
			"12CCMInterface",
			"RecvPkt",
			3,
			{3, "CCMInterface - received heartbeat response, connection still good"}
		};
	};

	namespace CGameInfoDialog
	{
		VFTableInfo_t ServerResponded
		{
			"15CGameInfoDialog",
			"ServerResponded",
			5,
			{5, "Pinged %s but got reply from %s?"},
			true
		};
	}

	namespace CSteamMatchmakingServers
	{
		// Neither function references a distinctive string; slot 18 (the
		// typed server-list request) does, and any shift that moves slot 0
		// or 7 moves it too.
		VFTableInfo_t GetServerDetails
		{
			"24CSteamMatchMakingServers",
			"GetServerDetails",
			7,
			{18, "Invalid server type"},
			true
		};
		VFTableInfo_t RequestInternetServerList
		{
			"24CSteamMatchMakingServers",
			"RequestInternetServerList",
			0,
			{18, "Invalid server type"},
			true
		};
	}

	namespace IClientApps
	{
		VFTableInfo_t GetAppData
		{
			"14IClientAppsMap",
			"GetAppData"
		};
		VFTableInfo_t GetAppType
		{
			"14IClientAppsMap",
			"GetAppType"
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
		VFTableInfo_t RequestAppInfoUpdate
		{
			"14IClientAppsMap",
			"RequestAppInfoUpdate"
		};
	}

	namespace IClientAppManager
	{
		VFTableInfo_t BCanRemotePlayTogether
		{
			"20IClientAppManagerMap",
			"BCanRemotePlayTogether"
		};
		VFTableInfo_t BIsDlcEnabled
		{
			"20IClientAppManagerMap",
			"BIsDlcEnabled"
		};
		VFTableInfo_t GetAppInstallState
		{
			"20IClientAppManagerMap",
			"GetAppInstallState"
		};
		VFTableInfo_t GetAppStateInfo
		{
			"20IClientAppManagerMap",
			"GetAppStateInfo"
		};
		VFTableInfo_t GetLibraryFolderLabel
		{
			"20IClientAppManagerMap",
			"GetLibraryFolderLabel"
		};
		VFTableInfo_t GetLibraryFolderPath
		{
			"20IClientAppManagerMap",
			"GetLibraryFolderPath"
		};
		VFTableInfo_t GetNumLibraryFolders
		{
			"20IClientAppManagerMap",
			"GetNumLibraryFolders"
		};
		VFTableInfo_t GetUpdateInfo
		{
			"20IClientAppManagerMap",
			"GetUpdateInfo"
		};
		VFTableInfo_t InstallApp
		{
			"20IClientAppManagerMap",
			"InstallApp"
		};
		VFTableInfo_t IsAppDlcInstalled
		{
			"20IClientAppManagerMap",
			"IsAppDlcInstalled"
		};
		VFTableInfo_t LaunchApp
		{
			"20IClientAppManagerMap",
			"LaunchApp"
		};
		VFTableInfo_t UninstallApp
		{
			"20IClientAppManagerMap",
			"UninstallApp"
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
		VFTableInfo_t BIsCompatLayerEnabled
		{
			"16IClientCompatMap",
			"BIsCompatLayerEnabled"
		};
		VFTableInfo_t BIsCompatibilityToolEnabled
		{
			"16IClientCompatMap",
			"BIsCompatibilityToolEnabled"
		};
		VFTableInfo_t GetCompatToolDisplayName
		{
			"16IClientCompatMap",
			"GetCompatToolDisplayName"
		};
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

	namespace IClientConfigStore
	{
		VFTableInfo_t SetString
		{
			"21IClientConfigStoreMap",
			"SetString"
		};
	}

	namespace IClientFriends
	{
		VFTableInfo_t GetFriendGamePlayed
		{
			"17IClientFriendsMap",
			"GetFriendGamePlayed"
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
		VFTableInfo_t GetAppId
		{
			"15IClientUtilsMap",
			"GetAppID"
		};
		VFTableInfo_t GetOfflineMode
		{
			"15IClientUtilsMap",
			"GetOfflineMode"
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
		VFTableInfo_t GetLegacyCDKey
		{
			"14IClientUserMap",
			"GetLegacyCDKey"
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
		VFTableInfo_t SetLegacyCDKey
		{
			"14IClientUserMap",
			"SetLegacyCDKey"
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

void VFTIndexes::dump(const std::string& name, const std::map<std::string, unsigned int>& functionMap)
{
	std::ostringstream ss;

	ss << "\n" << name << "\n{";

	for (const auto& kv : functionMap)
	{
		ss << "\n\t" << kv.first << " = " << kv.second;
	}

	ss << "\n};";

	LOG_INFO("Dump %s\n", ss.str().c_str());
}

bool VFTIndexes::init()
{
	LOG_DEBUG("VFTIndexes::init()\n");

	// Only optional hooks use these (Hooks::setup warns and drops the feature),
	// so a changed client string must not fail the whole load.
	IClientCompat::BIsCompatLayerEnabled.optional = true;
	IClientConfigStore::SetString.optional = true;
	IClientFriends::GetFriendGamePlayed.optional = true;
	IClientRemoteStorage::IsCloudEnabledForApp.optional = true;

	bool success = true;

	for(const auto& fn : functions())
	{
		if (!fn->init())
		{
			success = false;
		}
	}

	if (g_config.dumpInterfaceMaps.copy())
	{
		for (const auto& tbl : tableMap)
		{
			dump(tbl.first, tbl.second);
		}
	}

	return success;
}
