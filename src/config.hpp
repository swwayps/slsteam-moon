#pragma once

#include "mtvar.hpp"
#include "log.hpp"
#include "sdk/types.hpp"
#include "feats/manifestpins.hpp"
#include "feats/librarydates.hpp"

#include "yaml-cpp/exceptions.h"
#include "yaml-cpp/node/node.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


class CFileWatcher;

class CConfig {
public:
	typedef unsigned int SmartTicketsFlags_t;

	enum ESmartTickets : SmartTicketsFlags_t
	{
		k_ESmartTicketsSteamDRM = 1 << 0,
		k_ESmartTicketsDenuvo = 1 << 1,
	};

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

	struct DonateSettings
	{
		bool enabled = true;
		std::string url = "https://manifest.luastools.xyz";
		uint32_t intervalSecs = 30;
		uint32_t wantedRefreshSecs = 300;
		uint32_t maxMintsPerCycle = 25;
		uint32_t minMintIntervalMs = 2000;
		uint32_t maxMintsPerSession = 0;
	};

	static std::vector<std::uint32_t> selectConfiguredDlcIds(
		const std::unordered_set<std::uint32_t>& managedParents,
		const std::unordered_map<std::uint32_t, CDlcData>& configured)
	{
		std::vector<std::uint32_t> selected;
		for (const auto& [parentId, data] : configured)
		{
			if (parentId == 0 || !managedParents.contains(parentId)) continue;
			for (const auto& [dlcId, name] : data.dlcIds)
			{
				(void)name;
				if (dlcId != 0) selected.push_back(dlcId);
			}
		}
		std::sort(selected.begin(), selected.end());
		selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
		return selected;
	}

	enum class ELoadError : uint32_t
	{
		None,
		MissingKey,
		ParsingException
	};
	MTVariable<std::string> __loadErrors;

	MTVariable<std::unordered_set<uint32_t>> appIds;
	// Apps sourced from stplug-in/luaappids and eligible for appinfo fetching.
	MTVariable<std::unordered_set<uint32_t>> managedAppIds;
	// Managed apps plus installed compatibility entries used by ownership and
	// package hooks. Compatibility entries never enter the provider chain.
	MTVariable<std::unordered_set<uint32_t>> addedAppIds;
	// Last observed on-disk installed set (appmanifest + content). Baseline for
	// recovering a racy steamapps walk that transiently drops a still-installed
	// compatibility app, so a scan artifact is not mistaken for an uninstall.
	MTVariable<std::unordered_set<uint32_t>> installedAppIds;
	MTVariable<std::unordered_map<uint32_t, CDlcData>> dlcData;
	MTVariable<std::unordered_map<uint32_t, uint64_t>> appTokens;
	MTVariable<std::unordered_map<uint32_t, std::string>> cdKeys;
	MTVariable<std::unordered_set<uint32_t>> fakeOffline;
	MTVariable<std::unordered_map<uint32_t, uint32_t>> fakeAppIds;
	MTVariable<std::unordered_map<uint32_t, uint64_t>> manifestIds;
	MTVariable<std::unordered_set<uint32_t>> depotBlacklist;
	MTVariable<FakeGame_t> idleStatus;
	MTVariable<std::unordered_map<uint32_t, std::string>> gameTitles;
	MTVariable<std::unordered_map<uint32_t, uint32_t>> subscriptionTimestamps;
	LibraryDates::Store libraryDates;

	// Manifest pinning.  lockedApps drives shouldDisableUpdates;
	// manifestPinsByApp is the structured source for app-scoped lookup.
	MTVariable<std::unordered_set<uint32_t>> lockedApps;
	MTVariable<ManifestPins::PinMap> manifestPinsByApp;

	MTVariable<std::unordered_map<uint64_t, std::unordered_set<AppId_t>>> denuvoGames;
	MTVariable<std::unordered_map<AppId_t, uint64_t>> steamIdOverride;

	MTVariable<std::unordered_map<AppId_t, std::string>> launchOptions;

	MTVariable<bool> disableFamilyLock;
	MTVariable<bool> disableParentalRestrictions;
	MTVariable<bool> useWhiteList;
	MTVariable<SmartTicketsFlags_t> smartTickets;
	MTVariable<uint32_t> maxSchemaTries;
	MTVariable<bool> notifications;
	MTVariable<bool> notifyInit;
	MTVariable<bool> api;
	// Run Lua plugins from <config dir>/plugins (off by default: they run
	// arbitrary code inside Steam).
	MTVariable<bool> plugins;
	MTVariable<bool> disableCloud;
	// When off, fully-installed managed apps that are NOT individually pinned
	// freeze on their installed build instead of tracking the latest public
	// gid (mirrors LuaTools' "Auto Update Apps"). Per-app pins are unaffected.
	// Default on.
	MTVariable<bool> autoUpdateApps;
	MTVariable<DonateSettings> donate;
	// Restore the pre-Phase-3 behavior of injecting every advertised DLC
	// into package 0. Default false keeps storefront-only DLC out of CM
	// ownership traffic unless it has content of its own.
	MTVariable<bool> injectAllAdvertisedDlc;
	MTVariable<bool> achievements;
	MTVariable<uint64_t> achievementOwnerId;
	MTVariable<std::unordered_map<uint32_t, uint64_t>> achievementOwners;
	MTVariable<bool> disableUpdates;
	MTVariable<std::string> fakeName;
	MTVariable<std::string> fakeEmail;
	MTVariable<int32_t> fakeWalletBalance;
	MTVariable<unsigned int> logLevel;
	MTVariable<bool> patternCache;
	// Move refreshes for existing provisioned buffers off la_preinit. A
	// missing buffer still takes the synchronous PICS fallback.
	MTVariable<bool> asyncProvision;
	MTVariable<bool> dumpInterfaceMaps;
	MTVariable<bool> extendedLogging;
	// Safety valve for pathological bulk copies into stplug-in (see
	// config_discovery.hpp). This is NOT a library-size limit: it sits far above
	// any realistic library and only keeps a tens-of-thousands-of-scripts drop
	// from stalling the client. 0 disables the cap entirely.
	MTVariable<std::size_t> maxManagedApps;

	//Using incomplete class to avoid runtime linking errors
	CFileWatcher* watcher;

	// The Lua plugin API's view of config.yaml. Only Lua code reads or writes
	// it, under Lua::stateMutex.
	YAML::Node rootNode;
	// Set by the Lua runtime while plugins run. pluginConfigLoading gets every
	// freshly parsed config before its settings are applied and may change it
	// (SLSsteam::configLoading); pluginConfigLoaded runs after
	// (SLSsteam::configLoaded).
	std::atomic<void(*)(YAML::Node& node)> pluginConfigLoading {nullptr};
	std::atomic<void(*)()> pluginConfigLoaded {nullptr};

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

	void setError(const ELoadError err, const char* keyName);
	bool loadSettings();
	// The config watcher's reload: sources, removals, tickets and the hot-add
	// publication, not only loadSettings().
	void reload();
	// config.yaml parsed (and repaired in memory) without applying or writing
	// anything: the Lua plugin API's view before plugins first run.
	YAML::Node readTree();

	template<typename T>
	T getSetting(YAML::Node& node, const char* name, T defVal)
	{
		if (!node[name])
		{
			//g_pLog->notifyLong("Missing %s in configfile! Using default", name);
			setError(ELoadError::MissingKey, name);
			return defVal;
		}

		// An empty `Key:` is a null node, which yaml-cpp converts to the
		// string "null" (so an empty FakeName: would rename the persona).
		if (node[name].IsNull())
		{
			return defVal;
		}

		// Decode without throwing: a bad scalar (say, a huge
		// FakeWalletBalance) falls back to the default and is reported by key.
		T value {};
		if (!YAML::convert<T>::decode(node[name], value))
		{
			setError(ELoadError::ParsingException, name);
			return defVal;
		}
		return value;
	}

	template<typename T>
	std::unordered_set<T> getList(YAML::Node& rootNode, const char* name)
	{
		auto list = std::unordered_set<T>();

		const auto node = rootNode[name];
		if (!node)
		{
			//g_pLog->notifyLong("Missing %s in configfile! Using default", name);
			setError(ELoadError::MissingKey, name);
			return list;
		}

		for(auto subNode : node)
		{
			try
			{
				T val = subNode.as<T>();
				list.emplace(val);

				// Per entry at debug: the watcher reloads on every game add.
				if constexpr (std::is_same_v<T, uint32_t>)
				{
					g_pLog->debug("Added %u to %s\n", val, name);
				}
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse %s!", name);
				setError(ELoadError::ParsingException, name);
			}
		}

		if constexpr (std::is_same_v<T, uint32_t>)
		{
			g_pLog->info("Config: %s has %zu id(s)\n", name, list.size());
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
			setError(ELoadError::MissingKey, name);
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

				if constexpr (std::is_same_v<T, uint32_t> && std::is_same_v<T, T2>)
				{
					g_pLog->debug("Added %u to %u in %s\n", k, v, name);
				}
				else if constexpr (std::is_same_v<T, uint32_t> && std::is_same_v<T2, uint64_t>)
				{
					g_pLog->debug("Added %u to %llu in %s\n", k, static_cast<unsigned long long>(v), name);
				}
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse %s!", name);
				setError(ELoadError::ParsingException, name);
			}
		}

		return map;
	}

	bool isAddedAppId(uint32_t appId);
	// Lua plugin API: replace the active app set in memory, as upstream. Managed
	// apps (stplug-in, luaappids.yaml) stay active; the next reload resets it.
	void setAdditionalApps(const std::unordered_set<uint32_t>& appIds, bool firstLoad = false);

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

	bool shouldExcludeAppId(uint32_t appId, bool ignoreAdditionalApps = false);
	CSteamId getDenuvoGameOwner(const AppId_t appId);
};

extern CConfig g_config;
