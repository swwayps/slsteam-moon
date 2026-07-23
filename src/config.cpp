#include "config.hpp"

#include "confload.hpp"
#include "config_default.hpp"
#include "filewatcher.hpp"
#include "log.hpp"
#include "yaml-cpp/yaml.h"

#include "feats/depotkey.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	// Non-throwing uint64 parse for pin gids.  std::stoull THROWS on an empty /
	// non-numeric / overflowing string, and under the release build
	// (-O3 -flto -freorder-blocks-and-partition) such a throw can escape the
	// surrounding catch (the .cold-partition EH defect documented in
	// config.hpp), aborting the client at startup.  Returns false on any bad
	// input so the caller skips the entry instead of throwing.
	bool parsePinGid(const std::string& s, uint64_t& out)
	{
		if (s.empty()) return false;
		errno = 0;
		char* end = nullptr;
		const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
		if (errno != 0 || end == s.c_str() || *end != '\0') return false;
		out = static_cast<uint64_t>(v);
		return true;
	}

	// Read a whole file into `out`. Returns false if the file can't be opened.
	// Uses streams (no yaml-cpp), so it never throws a parser exception.
	bool readFileText(const std::string& path, std::string& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f.is_open()) return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	// Persist `text` to `path` atomically (write a sibling temp file, then
	// rename over the target) so a crash mid-write can never leave a truncated
	// config. Returns false on any IO error.
	bool writeFileTextAtomic(const std::string& path, const std::string& text)
	{
		const std::string tmp = path + ".slsheal.tmp";
		{
			std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
			if (!f.is_open()) return false;
			f << text;
			f.flush();
			if (!f.good()) return false;
		}
		std::error_code ec;
		std::filesystem::rename(tmp, path, ec);
		if (ec)
		{
			std::filesystem::remove(tmp, ec);
			return false;
		}
		return true;
	}
}


std::string CConfig::getDir()
{
	char pathBuf[255];
	const char* configDir = getenv("XDG_CONFIG_HOME"); //Most users should have this set iirc
	if (configDir != NULL)
	{
		sprintf(pathBuf, "%s/SLSsteam", configDir);
	}
	else
	{
		const char* home = getenv("HOME");
		sprintf(pathBuf, "%s/.config/SLSsteam", home);
	}

	return std::string(pathBuf);
}

std::string CConfig::getPath()
{
	return getDir().append("/config.yaml");
}

std::string CConfig::getLuaAppIdsPath()
{
	return getDir().append("/luaappids.yaml");
}

static std::string findSteamRootForConfig()
{
	const char* home = std::getenv("HOME");
	if (!home) return "";
	const std::vector<std::string> candidates = {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam"
	};
	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate + "/steam.sh"))
		{
			return candidate;
		}
	}
	return "";
}

// Discover installable MAIN apps from the SteamTools-format scripts under
// <Steam>/config/stplug-in.  A file is named after the app it unlocks
// (e.g. 250900.lua); everything INSIDE it describes that app's components
// (keyed `addappid(depot,1,"hex")` = depot keys, bare `addappid(dlc)` = DLC
// ownership grants, `setManifestid` = pins) and MUST NOT be treated as
// separate main apps — doing so inflates the set with hundreds of
// DLC/depot ids that can't be provisioned.  So the only reliable main-app
// signal is the numeric filename stem.  Depot keys are ingested separately
// by DepotKey::importLuaScripts; DLC appids are injected from the base
// app's appinfo.  This is the PRIMARY source for the new version.
std::unordered_set<uint32_t> CConfig::discoverStPluginAppIds()
{
	std::unordered_set<uint32_t> result;
	const auto steamRoot = findSteamRootForConfig();
	if (steamRoot.empty()) return result;

	const auto stplug = steamRoot + "/config/stplug-in";
	if (!std::filesystem::exists(stplug)) return result;

	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(stplug, ec))
	{
		if (!entry.is_regular_file()) continue;
		const auto& path = entry.path();
		if (path.extension() != ".lua") continue;

		try
		{
			uint32_t appIdStem = static_cast<uint32_t>(std::stoul(path.stem().string()));
			if (appIdStem > 0 && !DepotKey::isManagedDepot(appIdStem))
			{
				result.insert(appIdStem);
			}
		}
		catch (...) {}
	}
	return result;
}

// Read manual/plugin AdditionalApps overrides from luaappids.yaml.  This
// file is authored by the user or the LuaTools plugin; we only READ it (we
// never mirror the discovered set back into it, which is what previously
// let a bad pass persist as permanent pollution).  Managed depot ids are
// filtered out defensively.
std::unordered_set<uint32_t> CConfig::loadLuaAppIdsYaml()
{
	std::unordered_set<uint32_t> result;
	const auto path = getLuaAppIdsPath();
	if (!std::filesystem::exists(path)) return result;

	try
	{
		YAML::Node node = YAML::LoadFile(path);
		if (node && node["AdditionalApps"])
		{
			for (auto subNode : node["AdditionalApps"])
			{
				try
				{
					uint32_t val = subNode.as<uint32_t>();
					if (val > 0 && !DepotKey::isManagedDepot(val))
					{
						result.insert(val);
					}
				}
				catch (...) {}
			}
		}
	}
	catch (...) {}

	return result;
}

bool CConfig::createFile()
{
	std::string path = getPath();
	if (!std::filesystem::exists(path))
	{
		std::string dir = getDir();
		if (!std::filesystem::exists(dir))
		{
			if (!std::filesystem::create_directory(dir))
			{
				g_pLog->notify("Unable to create config directory at %s!\n", dir.c_str());
				g_pLog->notifyUser(UserMsg::ConfigWriteFailed);
				return false;
			}

			g_pLog->debug("Created config directory at %s\n", dir.c_str());
		}

		FILE* file = fopen(path.c_str(), "w");
		if (!file)
		{
			g_pLog->notify("Unable to create config at %s!\n", path.c_str());
			g_pLog->notifyUser(UserMsg::ConfigWriteFailed);
			return false;
		}

		fputs(defaultConfig, file);
		fflush(file);
		fclose(file);
	}

	return true;
}

static void onFileChange()
{
	g_config.loadSettings();
}

bool CConfig::init()
{
	if(createFile())
	{
		watcher = new CFileWatcher(onFileChange);
		watcher->addFile(getPath().c_str());

		// Also watch the new-version AdditionalApps sources so adding a game
		// (a .lua dropped into stplug-in, or an entry appended to
		// luaappids.yaml) hot-reloads without a Steam restart.
		watcher->addFile(getLuaAppIdsPath().c_str());
		const auto steamRoot = findSteamRootForConfig();
		if (!steamRoot.empty())
		{
			const auto stplug = steamRoot + "/config/stplug-in";
			if (std::filesystem::exists(stplug))
			{
				watcher->addFile(stplug.c_str());
			}
		}

		watcher->start();
	}

	loadSettings();
	return true;
}

CConfig::~CConfig()
{
	if (watcher)
	{
		delete watcher;
	}
}


void CConfig::setError(ELoadError err)
{
	if (__loadErrors.get() > err)
	{
		return;
	}

	__loadErrors = err;
}

bool CConfig::loadSettings()
{
	// Load config.yaml WITHOUT ever letting a parser exception abort the client.
	//
	// A malformed config (most commonly the LuaTools AdditionalApps writer
	// leaving inconsistent list indentation) used to make YAML::LoadFile throw
	// YAML::ParserException; under the release build that throw can escape the
	// catch (the .cold-partition EH defect documented in config.hpp) and abort
	// Steam at startup -> boot loop. We now:
	//   1. read the file as plain text (no throw),
	//   2. hand it to ConfLoad::parseWithRepair, which normalises inconsistent
	//      block-sequence indentation and parses the fixed text FIRST (so the
	//      throwing path isn't even reached for the common breakage), never
	//      throwing itself,
	//   3. self-heal the file on disk when a repair was applied, so the user's
	//      game list survives and the file is clean for the FileWatcher/plugin,
	//   4. fall back to built-in defaults (and boot) if nothing parses.
	YAML::Node node;
	std::string raw;
	if (!readFileText(getPath(), raw))
	{
		g_pLog->notifyLong("Can not read config.yaml!\nUsing defaults");
		g_pLog->notifyUser(UserMsg::ConfigUnreadable);
		node = YAML::Node(); //Create empty node and let defaults kick in
	}
	else
	{
		std::string repaired;
		const ConfLoad::Outcome outcome =
		    ConfLoad::parseWithRepair(raw, node, repaired);

		if (outcome == ConfLoad::Outcome::Failed)
		{
			g_pLog->notifyLong("Error parsing config.yaml!\nUsing defaults");
			g_pLog->notifyUser(UserMsg::ConfigParseFailed);
			node = YAML::Node(); //Create empty node and let defaults kick in
		}
		else if (outcome == ConfLoad::Outcome::Repaired)
		{
			if (writeFileTextAtomic(getPath(), repaired))
				g_pLog->notify("Config had inconsistent list indentation; auto-repaired on disk\n");
			else
				g_pLog->notify("Config had inconsistent list indentation; repaired in memory (disk write failed)\n");
			g_pLog->notifyUser(UserMsg::ConfigRepaired);
		}
	}

	__loadErrors = ELoadError::None;
	
	disableFamilyLock = getSetting<bool>(node, "DisableFamilyShareLock", true);
	useWhiteList = getSetting<bool>(node, "UseWhitelist", false);
	automaticFilter = getSetting<bool>(node, "AutoFilterList", true);
	playNotOwnedGames = getSetting<bool>(node, "PlayNotOwnedGames", false);
	// SafeMode (abort the load on an unknown steamclient.so hash) is force-
	// disabled. Its hash whitelist cannot be kept current: it goes stale on
	// every Steam client update and would disable an otherwise-working client,
	// and it is sourced from an upstream mirror we do not control. The Steam
	// wrapper's crash-loop fail-safe (setup.sh) now covers the Game Mode brick
	// scenario SafeMode guarded, recovering on the first crash after a client
	// change. The key is still read so an existing "SafeMode: yes" neither
	// errors nor gates the load; the feature code is kept intact, just inert.
	(void)getSetting<bool>(node, "SafeMode", false);
	safeMode = false;
	notifications = getSetting<bool>(node, "Notifications", true);
	warnHashMissmatch = getSetting<bool>(node, "WarnHashMissmatch", false);
	notifyInit = getSetting<bool>(node, "NotifyInit", true);
	api = getSetting<bool>(node, "API", true);
	fakeEmail = getSetting<std::string>(node, "FakeEmail", "");
	fakeWalletBalance = getSetting<int32_t>(node, "FakeWalletBalance", 0);
	disableCloud = getSetting<bool>(node, "DisableCloud", true);
	achievements = getSetting<bool>(node, "Achievements", true);
	achievementOwnerId = getSetting<uint64_t>(node, "AchievementOwnerId", 76561198028121353ULL);
	extendedLogging = getSetting<bool>(node, "ExtendedLogging", false);
	logLevel = getSetting<unsigned int>(node, "LogLevel", 2);

	//TODO: Create smart logging function to log them automatically via getSetting
	g_pLog->info("DisableFamilyShareLock: %i\n", disableFamilyLock.get());
	g_pLog->info("UseWhitelist: %i\n", useWhiteList.get());
	g_pLog->info("AutoFilterList: %i\n", automaticFilter.get());
	g_pLog->info("PlayNotOwnedGames: %i\n", playNotOwnedGames.get());
	g_pLog->info("SafeMode: %i\n", safeMode.get());
	g_pLog->info("Notifications: %i\n", notifications.get());
	g_pLog->info("WarnHashMissmatch: %i\n", warnHashMissmatch.get());
	g_pLog->info("NotifyInit: %i\n", notifyInit.get());
	g_pLog->info("API: %i\n", api.get());
	g_pLog->info("FakeEmail: %s\n", fakeEmail.get().c_str());
	g_pLog->info("FakeWalletBalance: %i\n", fakeWalletBalance.get());
	g_pLog->info("DisableCloud: %i\n", disableCloud.get());
	g_pLog->info("Achievements: %i\n", achievements.get());
	g_pLog->info("ExtendedLogging: %i\n", extendedLogging.get());
	g_pLog->info("LogLevel: %i\n", logLevel.get());

	appIds = getList<uint32_t>(node, "AppIds");

	// AdditionalApps is the UNION of three sources:
	//   1. stplug-in/*.lua stems      — primary source for the new version
	//   2. luaappids.yaml              — manual / plugin overrides
	//   3. config.yaml AdditionalApps  — LEGACY: everything an upgrading user
	//                                    already had lives here, and may not
	//                                    exist under stplug-in, so we must keep
	//                                    honouring it.
	{
		auto stplugApps  = discoverStPluginAppIds();
		auto luaYamlApps = loadLuaAppIdsYaml();
		auto legacyApps  = getList<uint32_t>(node, "AdditionalApps");

		std::unordered_set<uint32_t> combined;
		combined.insert(stplugApps.begin(),  stplugApps.end());
		combined.insert(luaYamlApps.begin(), luaYamlApps.end());
		combined.insert(legacyApps.begin(),  legacyApps.end());

		g_pLog->info("AdditionalApps sources: stplug-in=%zu luaappids.yaml=%zu "
		             "config.yaml(legacy)=%zu -> total=%zu\n",
		             stplugApps.size(), luaYamlApps.size(), legacyApps.size(),
		             combined.size());

		addedAppIds = combined;
	}

	fakeOffline = getList<uint32_t>(node, "FakeOffline");

	fakeAppIds = getMap<uint32_t, uint32_t>(node, "FakeAppIds");
	appTokens = getMap<uint32_t, uint64_t>(node, "AppTokens");
	achievementOwners = getMap<uint32_t, uint64_t>(node, "AchievementOwners");
	gameTitles = getMap<uint32_t, std::string>(node, "GameTitles");
	subscriptionTimestamps = getMap<uint32_t, uint32_t>(node, "SubscriptionTimestamps");

	//Do not warn for these (yet?)
	const auto idleStatusNode = node["IdleStatus"];
	if (idleStatusNode)
	{
		try
		{
			auto appId = idleStatusNode["AppId"].as<uint32_t>();
			auto title = idleStatusNode["Title"].as<std::string>();

			idleStatus = FakeGame_t
			{
				appId,
				title
			};

			g_pLog->info("Idle status %s with AppId %u\n", title.c_str(), appId);
		}
		catch(...)
		{
			//g_pLog->warn("Failed to parse IdleStatus!");A
			setError(ELoadError::ParsingException);
		}
	}

	const auto dlcDataNode = node["DlcData"];
	if(dlcDataNode)
	{
		auto _dlcData = dlcData.empty();

		for(auto& app : dlcDataNode)
		{
			try
			{
				const uint32_t parentId = app.first.as<uint32_t>();

				CDlcData data;
				data.parentId = parentId;
				g_pLog->info("Adding DlcData for %u\n", parentId);

				for(auto& dlc : app.second)
				{
					const uint32_t dlcId = dlc.first.as<uint32_t>();
					//There's more efficient types to store strings, but they mostly do not work
					const std::string dlcName = dlc.second.as<std::string>();

					data.dlcIds[dlcId] = dlcName;
					g_pLog->info("DlcId %u -> %s\n", dlcId, dlcName.c_str());
				}

				_dlcData[parentId] = data;
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse DlcData!");
				setError(ELoadError::ParsingException);
				break;
			}
		}

		dlcData = _dlcData;
	}
	else
	{
		//g_pLog->notify("Missing DlcData entry in config!");
		setError(ELoadError::MissingKey);
	}

	const auto denuvoGamesNode = node["DenuvoGames"];
	if (denuvoGamesNode)
	{
		auto _denuvoGames = denuvoGames.empty();

		for (auto& steamIdNode : denuvoGamesNode)
		{
			try
			{
				const uint32_t steamId = steamIdNode.first.as<uint32_t>();
				_denuvoGames[steamId] = std::unordered_set<uint32_t>();

				for (auto& appIdNode : steamIdNode.second)
				{
					const uint32_t appId = appIdNode.as<uint32_t>();
					_denuvoGames[steamId].emplace(appId);

					//Again, not loggin SteamId because of privacy
					g_pLog->info("Added DenuvoGame %u\n", appId);
				}
			}
			catch (...)
			{
				//g_pLog->notify("Failed to parse DenuvoGames!");
				setError(ELoadError::ParsingException);
			}
		}

		denuvoGames.set(_denuvoGames);
	}
	else
	{
		//g_pLog->notify("Missing DenuvoGames entry in config!");
		setError(ELoadError::MissingKey);
	}

	// ManifestPins: nested  appid -> { locked, depots: {depot: "gid"} }.
	// gids are STRINGS (uint64 exceeds YAML int safety).  Missing key is fine.
	//
	// Parse defensively with non-throwing accessors and explicit node-type
	// guards: a malformed block (a scalar where a map is expected, a stray
	// "<depot>: gid" line at app level, a non-numeric gid) must be SKIPPED, not
	// thrown.  The catch (...) below is a last resort only -- under the release
	// build a yaml-cpp/std::stoull throw can escape it (the .cold-partition EH
	// defect noted in config.hpp), which is exactly what aborts the client at
	// startup, so we must not rely on it for routine bad input.
	{
		ManifestPins::PinMap pinMap;
		const auto pinsNode = node["ManifestPins"];
		if (pinsNode && pinsNode.IsMap())
		{
			for (auto& appNode : pinsNode)
			{
				try
				{
					const auto& idNode = appNode.first;
					const auto& body = appNode.second;

					// The app key must be a number; skip junk keys.
					const uint32_t appId =
					    idNode.IsScalar() ? idNode.as<uint32_t>(0) : 0;
					if (appId == 0) continue;

					// A pin entry is a map { locked, depots, ... }.  A scalar
					// value here means a malformed/legacy line (e.g. a stray
					// "<depot>: gid" emitted at app level) -- skip it instead of
					// indexing a non-map node, which can throw.
					if (!body.IsMap()) continue;

					ManifestPins::AppPins app;
					app.locked = body["locked"].as<bool>(false);
					app.buildId = body["build_id"].as<uint32_t>(0);

					const auto depotsNode = body["depots"];
					if (depotsNode.IsMap())
					{
						for (auto& d : depotsNode)
						{
							if (!d.first.IsScalar() || !d.second.IsScalar())
								continue;
							const uint32_t depotId = d.first.as<uint32_t>(0);
							if (depotId == 0) continue;
							uint64_t gid = 0;
							if (parsePinGid(d.second.as<std::string>(""), gid) &&
							    gid != 0)
							{
								app.depots[depotId] = gid;
							}
						}
					}
					pinMap[appId] = app;
				}
				catch (...)
				{
					setError(ELoadError::ParsingException);
				}
			}
		}

		// Belt-and-suspenders: drop pins for apps no longer in
		// AdditionalApps (plugin remove-game is the primary purge).
		ManifestPins::purgeOrphans(pinMap, addedAppIds.get());

		manifestPinsByApp.set(pinMap);
		manifestPins.set(ManifestPins::flattenDepots(pinMap));
		lockedApps.set(ManifestPins::lockedAppSet(pinMap));
	}

	switch(__loadErrors.get())
	{
		case ELoadError::MissingKey:
			g_pLog->notify("Issues during config loading encountered! Missing key(s)");
			break;
		case ELoadError::ParsingException:
			g_pLog->notify("Issues during config loading encountered! Parsing error(s)");
			break;

		default:
			break;
	}


	return true;
}

bool CConfig::isAddedAppId(uint32_t appId)
{
	return addedAppIds.get().contains(appId);
}

uint64_t CConfig::getManifestPin(uint32_t depotId)
{
	return ManifestPins::getPin(manifestPins.get(), depotId);
}

bool CConfig::isAppLocked(uint32_t appId)
{
	return ManifestPins::isLocked(lockedApps.get(), appId);
}

std::unordered_map<uint32_t, uint64_t>
CConfig::getAppPinnedDepots(uint32_t appId)
{
	const auto pinMap = manifestPinsByApp.get();
	const auto it = pinMap.find(appId);
	if (it == pinMap.end()) return {};
	return it->second.depots;
}

void CConfig::purgePinsForApps(const std::unordered_set<uint32_t>& appIds)
{
	auto pinMap = manifestPinsByApp.get();
	ManifestPins::purgeApps(pinMap, appIds);
	manifestPinsByApp.set(pinMap);
	manifestPins.set(ManifestPins::flattenDepots(pinMap));
	lockedApps.set(ManifestPins::lockedAppSet(pinMap));
}

bool CConfig::shouldExcludeAppId(uint32_t appId)
{
	bool exclude = false;
	//Proper way would be with getAppType, but that seems broken so we need to do this instead
	constexpr uint32_t ONE_BILLION = 1E9; //Implicit cast from double to unsigned int, hopefully this does not break anything
	if (appId >= ONE_BILLION) //Higher and equal to 10^9 gets used by Steam Internally
	{
		exclude = true;
	}
	else
	{
		bool found = appIds.get().contains(appId);
		exclude = !isAddedAppId(appId) && ((useWhiteList.get() && !found) || (!useWhiteList.get() && found));
	}

	g_pLog->debugOnce("shouldExcludeAppId(%u) -> %i\n", appId, exclude);
	return exclude;
}

uint32_t CConfig::getDenuvoGameOwner(uint32_t appId)
{
	for(const auto& tpl : denuvoGames.get())
	{
		if (tpl.second.contains(appId))
		{
			//g_pLog->once("%u is DenuvoGame\n", appId);
			return tpl.first;
		}
	}

	return 0;
}

CConfig g_config = CConfig();
