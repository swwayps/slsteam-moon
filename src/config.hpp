#pragma once

#include "mtvar.hpp"
#include "log.hpp"
#include "feats/manifestpins.hpp"
#include "feats/librarydates.hpp"

#include "yaml-cpp/exceptions.h"
#include "yaml-cpp/node/node.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>


class CFileWatcher;

class CConfig {
public:
	struct FakeGame_t
	{
		uint32_t appId = 0;
		std::string title;
	};

	class CDlcData
	{
	public:
		uint32_t parentId;
		std::unordered_map<uint32_t, std::string> dlcIds;
		//No default constructor, otherwise dlcData will complain that no matching one was found
		//without implementing it ourself anyway
	};

	enum class ELoadError : uint32_t
	{
		None,
		MissingKey,
		ParsingException
	};
	MTVariable<ELoadError> __loadErrors;

	MTVariable<std::unordered_set<uint32_t>> appIds;
	// Apps sourced from stplug-in/luaappids and eligible for appinfo fetching.
	MTVariable<std::unordered_set<uint32_t>> managedAppIds;
	// Managed apps plus installed compatibility entries used by ownership and
	// package hooks. Compatibility entries never enter the provider chain.
	MTVariable<std::unordered_set<uint32_t>> addedAppIds;
	MTVariable<std::unordered_map<uint32_t, CDlcData>> dlcData;
	MTVariable<std::unordered_map<uint32_t, uint64_t>> appTokens;
	MTVariable<std::unordered_set<uint32_t>> fakeOffline;
	MTVariable<std::unordered_map<uint32_t, uint32_t>> fakeAppIds;
	MTVariable<FakeGame_t> idleStatus;
	MTVariable<std::unordered_map<uint32_t, std::string>> gameTitles;
	MTVariable<std::unordered_map<uint32_t, uint32_t>> subscriptionTimestamps;
	LibraryDates::Store libraryDates;

	// Manifest pinning.  manifestPins is the flattened, unambiguous
	// depot->gid redirect index used only by legacy hooks without app context;
	// lockedApps drives shouldDisableUpdates; manifestPinsByApp is the
	// structured source kept for app-scoped lookup and purge.
	MTVariable<std::unordered_map<uint32_t, uint64_t>> manifestPins;
	MTVariable<std::unordered_set<uint32_t>> lockedApps;
	MTVariable<ManifestPins::PinMap> manifestPinsByApp;

	MTVariable<std::unordered_map<uint32_t, std::unordered_set<uint32_t>>> denuvoGames;

	MTVariable<bool> disableFamilyLock;
	MTVariable<bool> disableParentalRestrictions;
	MTVariable<bool> useWhiteList;
	MTVariable<bool> automaticFilter;
	MTVariable<bool> playNotOwnedGames;
	MTVariable<bool> safeMode;
	MTVariable<bool> notifications;
	MTVariable<bool> warnHashMissmatch;
	MTVariable<bool> notifyInit;
	MTVariable<bool> api;
	MTVariable<bool> disableCloud;
	// Restore the pre-Phase-3 behavior of injecting every advertised DLC
	// into package 0. Default false keeps storefront-only DLC out of CM
	// ownership traffic unless it has content of its own.
	MTVariable<bool> injectAllAdvertisedDlc;
	MTVariable<bool> achievements;
	MTVariable<uint64_t> achievementOwnerId;
	MTVariable<std::unordered_map<uint32_t, uint64_t>> achievementOwners;
	MTVariable<std::string> fakeEmail;
	MTVariable<int32_t> fakeWalletBalance;
	MTVariable<unsigned int> logLevel;
	MTVariable<bool> patternCache;
	// Move refreshes for existing provisioned buffers off la_preinit. A
	// missing buffer still takes the synchronous PICS fallback.
	MTVariable<bool> asyncProvision;
	MTVariable<bool> extendedLogging;
	// Safety valve for pathological bulk copies into stplug-in (see
	// config_discovery.hpp). This is NOT a library-size limit: it sits far above
	// any realistic library and only keeps a tens-of-thousands-of-scripts drop
	// from stalling the client. 0 disables the cap entirely.
	MTVariable<std::size_t> maxManagedApps;

	//Using incomplete class to avoid runtime linking errors
	CFileWatcher* watcher;

	~CConfig();

	std::string getDir();
	std::string getPath();
	std::string getLuaAppIdsPath();
	bool createFile();
	bool init();

	// Managed-app sources (unioned in loadSettings / hot-reload):
	//   1. stplug-in/*.lua numeric filename stems
	//   2. luaappids.yaml AdditionalApps
	// Installed Accela and legacy config entries are added only to addedAppIds.
	std::unordered_set<uint32_t> discoverStPluginAppIds();
	std::unordered_set<uint32_t> loadLuaAppIdsYaml();

	void setError(ELoadError err);
	bool loadSettings();

	template<typename T>
	T getSetting(YAML::Node& node, const char* name, T defVal)
	{
		if (!node[name])
		{
			//g_pLog->notifyLong("Missing %s in configfile! Using default", name);
			setError(ELoadError::MissingKey);
			return defVal;
		}

		// Use yaml-cpp's NON-THROWING conversion (the as<T>(fallback) overload):
		// it returns defVal on a bad/out-of-range scalar instead of throwing
		// YAML::TypedBadConversion<T>.  We can NOT rely on catching that throw:
		// under the release build (-O3 -flto -freorder-blocks-and-partition)
		// the throw lives in the function's ".cold" partition and the call-site
		// table fails to route it to the catch landing pad, so even catch (...)
		// is bypassed -> the exception escapes loadSettings and aborts the
		// client at startup (a huge FakeWalletBalance bricked Steam on every
		// launch).  Not throwing at all sidesteps the partitioned-EH defect.
		return node[name].as<T>(defVal);
	};

	template<typename T>
	std::unordered_set<T> getList(YAML::Node& rootNode, const char* name)
	{
		auto list = std::unordered_set<T>();

		const auto node = rootNode[name];
		if (!node)
		{
			//g_pLog->notifyLong("Missing %s in configfile! Using default", name);
			setError(ELoadError::MissingKey);
			return list;
		}

		for(auto subNode : node)
		{
			try
			{
				T val = subNode.as<T>();
				list.emplace(val);

				//TODO: Find better way to log shit
				if (std::is_same_v<T, uint32_t>)
				{
					g_pLog->info("Added %u to %s\n", val, name);
				}
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse %s!", name);
				setError(ELoadError::ParsingException);
			}
		}

		return list;
	}

	template<typename T, typename T2>
	std::unordered_map<T, T2> getMap(YAML::Node& rootNode, const char* name)
	{
		auto map = std::unordered_map<T, T2>();

		const auto node = rootNode[name];
		if (!node)
		{
			//g_pLog->notifyLong("Missing %s in configfile! Using default", name);
			setError(ELoadError::MissingKey);
			return map;
		}

		for(auto& subNode : node)
		{
			try
			{
				//TODO: Add error checks for failed parsing since yaml-cpp does not throw
				auto k = subNode.first.as<T>();
				auto v = subNode.second.as<T2>();

				map[k] = v;

				if (std::is_same_v<T, uint32_t> && std::is_same_v<T, T2>)
				{
					g_pLog->info("Added %u to %u in %s\n", k, v, name);
				}
				else if (std::is_same_v<T, uint32_t> && std::is_same_v<T2, uint64_t>)
				{
					g_pLog->info("Added %u to %llu in %s\n", k, v, name);
				}
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse %s!", name);
				setError(ELoadError::ParsingException);
			}
		}

		return map;
	}

	bool isAddedAppId(uint32_t appId);
	bool addAdditionalAppId(uint32_t appId);

	// Legacy fallback for hooks that do not know the owning app.  Conflicting
	// depot ownership is omitted from this index and returns zero.
	uint64_t getManifestPin(uint32_t depotId);
	// Authoritative lookup for consumers that have the owning app context.
	uint64_t getManifestPin(uint32_t appId, uint32_t depotId);
	// Planner lookup: use DepotEntry's app context, with a fallback only when
	// the structured map proves that exactly one app owns the depot.
	uint64_t getManifestPinForPlanner(uint32_t appId, uint32_t depotId);
	bool isAppLocked(uint32_t appId);
	// The depot->gid pins for one app (empty if none).  Used to check whether
	// an app's installed depots already match its pins (pin-aware update
	// suppression: allow the one downgrade, then freeze).
	std::unordered_map<uint32_t, uint64_t> getAppPinnedDepots(uint32_t appId);
	void purgePinsForApps(const std::unordered_set<uint32_t>& appIds);

	bool shouldExcludeAppId(uint32_t appId);
	uint32_t getDenuvoGameOwner(uint32_t appId);
};

extern CConfig g_config;
