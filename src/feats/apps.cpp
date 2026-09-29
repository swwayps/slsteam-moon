#include "apps.hpp"
#include "stats_policy.hpp"

#include "stats_policy.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/CUtl.hpp"
#include "../sdk/IClientApps.hpp"
#include "../sdk/IClientAppManager.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "appinfo_provision.hpp"
#include "appinfostate.hpp"
#include "fakeappid.hpp"
#include "synthmark.hpp"
#include "../utils/ManifestFetch.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>
#include <string>
#include <string_view>

namespace
{
	// --- pin-aware update suppression helpers (apps.cpp-local) -------------
	//
	// A locked, pinned app must let Steam run the update ONCE to install the
	// pinned build, then freeze.  If we suppress unconditionally Steam never
	// applies the pin (it stays on the installed/public build); if we never
	// suppress Steam loops forever reconciling the pinned gid against appinfo's
	// public gid.  So suppress IFF the app's installed depots already match its
	// pins.  That needs the installed manifest gids, read from the app's
	// appmanifest_<appId>.acf across all Steam library folders.

	std::string steamRootForManifests()
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};
		const std::string roots[] = {
			std::string(home) + "/.steam/steam",
			std::string(home) + "/.steam/debian-installation",
			std::string(home) + "/.local/share/Steam",
		};
		for (const auto& r : roots)
		{
			struct stat st{};
			if (stat((r + "/steamapps/libraryfolders.vdf").c_str(), &st) == 0)
				return r;
		}
		return {};
	}

	// All library steamapps dirs (main root + every "path" in libraryfolders).
	std::vector<std::string> librarySteamappsDirs()
	{
		std::vector<std::string> dirs;
		const std::string root = steamRootForManifests();
		if (root.empty()) return dirs;
		dirs.push_back(root + "/steamapps");

		std::ifstream f(root + "/steamapps/libraryfolders.vdf");
		if (!f) return dirs;
		std::string line;
		while (std::getline(f, line))
		{
			// "path"  "/some/library"
			const auto k = line.find("\"path\"");
			if (k == std::string::npos) continue;
			const auto q1 = line.find('"', k + 6);
			if (q1 == std::string::npos) continue;
			const auto q2 = line.find('"', q1 + 1);
			if (q2 == std::string::npos) continue;
			dirs.push_back(line.substr(q1 + 1, q2 - q1 - 1) + "/steamapps");
		}
		return dirs;
	}

	std::string findAppManifestPath(uint32_t appId)
	{
		const std::string name = "/appmanifest_" + std::to_string(appId) + ".acf";
		for (const auto& d : librarySteamappsDirs())
		{
			const std::string p = d + name;
			struct stat st{};
			if (stat(p.c_str(), &st) == 0) return p;
		}
		return {};
	}

	// depot -> installed manifest gid, parsed from the .acf InstalledDepots
	// block.  Each depot block opens with `"<depot>" {` and (per Steam's
	// writer) lists `"manifest" "<gid>"` first; no nested braces inside.
	std::unordered_map<uint32_t, uint64_t> installedDepotGids(uint32_t appId)
	{
		std::unordered_map<uint32_t, uint64_t> out;
		const std::string path = findAppManifestPath(appId);
		if (path.empty()) return out;

		std::ifstream f(path);
		if (!f) return out;
		std::stringstream ss;
		ss << f.rdbuf();
		const std::string s = ss.str();

		const auto idStart = s.find("\"InstalledDepots\"");
		if (idStart == std::string::npos) return out;
		// Scope to the InstalledDepots block: from its '{' to the matching '}'.
		auto pos = s.find('{', idStart);
		if (pos == std::string::npos) return out;
		int depth = 1;
		size_t i = pos + 1;
		uint32_t curDepot = 0;
		while (i < s.size() && depth > 0)
		{
			const char c = s[i];
			if (c == '{') { ++depth; ++i; continue; }
			if (c == '}') { --depth; ++i; continue; }
			if (c == '"')
			{
				const auto end = s.find('"', i + 1);
				if (end == std::string::npos) break;
				const std::string tok = s.substr(i + 1, end - i - 1);
				i = end + 1;
				if (depth == 1)
				{
					// depot id key
					try { curDepot = static_cast<uint32_t>(std::stoul(tok)); }
					catch (...) { curDepot = 0; }
				}
				else if (depth == 2 && tok == "manifest" && curDepot)
				{
					const auto v1 = s.find('"', i);
					if (v1 == std::string::npos) break;
					const auto v2 = s.find('"', v1 + 1);
					if (v2 == std::string::npos) break;
					try {
						out[curDepot] =
						    std::stoull(s.substr(v1 + 1, v2 - v1 - 1));
					} catch (...) {}
					i = v2 + 1;
				}
				continue;
			}
			++i;
		}
		return out;
	}

	// True iff every pinned depot Steam ACTUALLY installs is on its pinned
	// gid.  A pin map can legitimately list depots Steam never mounts for this
	// machine: a depot with no usable key, a different-OS depot, or one pruned
	// from the public branch / dropped in a newer build than the pinned one.
	// Such a depot is never in InstalledDepots, so requiring EVERY pinned depot
	// to be installed made this return false forever -> shouldDisableUpdates
	// never froze the app -> Steam re-planned endlessly (the scheduled/
	// unscheduled "Update Required" loop, content_log "0 updated files").
	// Only the pinned depots that are installed must match; require at least
	// one so a not-yet-installed app doesn't freeze vacuously.
	bool appAtPinnedGids(uint32_t appId)
	{
		const auto pins = g_config.getAppPinnedDepots(appId);
		if (pins.empty()) return false;
		const auto installed = installedDepotGids(appId);
		if (installed.empty()) return false;

		bool anyInstalledPinMatched = false;
		for (const auto& [depot, gid] : pins)
		{
			const auto it = installed.find(depot);
			if (it == installed.end()) continue;   // depot not installed here
			if (it->second != gid) return false;    // installed but wrong gid
			anyInstalledPinMatched = true;
		}
		return anyInstalledPinMatched;
	}

	bool stripInstallEligible(uint32_t appId)
	{
		const bool appManagerResolved = g_pClientAppManager != nullptr;
		bool fullyInstalled = false;
		if (appManagerResolved)
		{
			fullyInstalled = (g_pClientAppManager->getAppInstallState(appId) &
			                  k_EAppStateFullyInstalled) != 0;
		}
		// cachePairMtimeSecs() is a two-stat probe of the on-disk pair; it takes
		// no lock and cannot be withdrawn by a validation miss, so it keeps the
		// filter armed when the published set is incomplete.
		const bool locallyAuthoritative = SynthMark::localAppInfoAuthority(
		    AppInfoState::isAuthoritative(appId),
		    AppInfoProvision::cachePairMtimeSecs(appId) > 0);
		return SynthMark::installStateAllowsStrip(
		    locallyAuthoritative,
		    g_config.isAddedAppId(appId), appManagerResolved, fullyInstalled);
	}

	bool permitSynthStrip(uint32_t appId)
	{
		return stripInstallEligible(appId);
	}
}

bool Apps::applistRequested;
MTVariable<std::unordered_set<AppId_t>> Apps::privateApps;

bool Apps::unlockApp(uint32_t appId, AppOwnershipInfo_t* info, const CSteamId& ownerId)
{
	//Changing the purchased field is enough, but just for nicety in the Steamclient UI we change the owner too
	info->owner = ownerId.accountId();
	info->realOwner = 0;
	info->familyShared = info->owner != g_currentSteamId.accountId();

	info->licensePermanent = !info->familyShared;
	info->retailLicense = false;
	info->licenseExpired = false;
	info->licensePending = false;
	info->licenseLocked = false;

	info->releaseState = k_EAppReleaseStateReleased;
	info->ownsLicense = true;

	info->lowViolence = false;
	info->regionRestricted = false;

	info->autoGrant = false;
	info->trialTime = 0;
	info->fromFreeWeekend = false;
	info->freeLicense = info->familyShared;
	info->siteLicense = false;

	g_pLog->infoOnce("Unlocked %u\n", appId);
	return true;
}

bool Apps::unlockApp(uint32_t appId, AppOwnershipInfo_t* info)
{
	return unlockApp(appId, info, g_currentSteamId);
}

void Apps::buildDepotDependency(uint32_t appId, CUtlVector<DepotInfo_t>* depots, CUtlVector<DepotInfo_t>* sharedDepots)
{
	LOG_DEBUG("Vec Alloc %u, Grow %u, Size %u\n", depots->memory.alloc, depots->memory.growSize, depots->size);

	const auto depotBlacklist = g_config.depotBlacklist.get();
	const auto manifestOverrides = g_config.manifestIds.get();

	for (unsigned int i = 0; i < depots->size; i++)
	{
		const auto depot = depots->at(i);

		if (depotBlacklist.contains(depot->depotId))
		{
			LOG_DEBUG("Removing %u with %llu\n", depot->depotId, depot->manifestId);
			depots->swap(i, depots->size - 1);
			depots->size--;
		}

		if (manifestOverrides.contains(depot->depotId))
		{
			// ManifestPins own the depots they name: the download manager
			// stages and plans that exact gid later, so a second, global
			// override here would plan a build other than the staged one.
			if (g_config.getManifestPinForPlanner(appId, depot->depotId))
			{
				g_pLog->debug("ManifestIds entry for %u skipped, ManifestPins owns it\n", depot->depotId);
			}
			else
			{
				const uint64_t oldId = depot->manifestId;
				depot->manifestId = manifestOverrides.at(depot->depotId);
				g_pLog->debug("Overrode %u's manifest %llu with %llu\n", depot->depotId, oldId, depot->manifestId);
			}
		}

		LOG_DEBUG("Depot %u for %u -> %llu\n", depot->depotId, depot->appId, depot->manifestId);
	}

	for (unsigned int i = 0; i < sharedDepots->size; i++)
	{
		const auto depot = sharedDepots->at(i);
		LOG_DEBUG("Shared Depot %u for %u -> %llu\n", depot->depotId, depot->appId, depot->manifestId);
	}

}

bool Apps::checkAppOwnership(uint32_t appId, AppOwnershipInfo_t* pInfo)
{
	if (!applistRequested || !pInfo || !g_currentSteamId.isSet())
	{
		return false;
	}

	const CSteamId denuvoOwner = g_config.getDenuvoGameOwner(appId);

	//Do not modify Denuvo enabled Games
	if (denuvoOwner.isSet() && denuvoOwner.steamId64 != g_currentSteamId.steamId64)
	{
		g_pLog->infoOnce("Skipping %u because it's a Denuvo game from someone else\n", appId);
		return false;
	}

	if (g_config.shouldExcludeAppId(appId))
	{
		return false;
	}
	if (StatsPolicy::hasNativeLicense(appId))
	{
		return false;
	}

	// Only LuaTools-managed apps are unlocked; everything else keeps Steam's
	// answer untouched.
	if (!g_config.isAddedAppId(appId))
	{
		return false;
	}

	if (pInfo->lowViolence)
	{
		pInfo->lowViolence = false;
		g_pLog->infoOnce("Decensoring %u\n", appId);
	}
	if (pInfo->regionRestricted)
	{
		pInfo->regionRestricted = false;
		g_pLog->infoOnce("Bypassing region restriction for %u\n", appId);
	}

	const auto times = g_config.subscriptionTimestamps.get();
	if (times.contains(appId))
	{
		pInfo->purchaseTime = times.at(appId);
	}
	else if (pInfo->subId == 0 ||
	         (pInfo->subId == -1 && !pInfo->ownsLicense))
	{
		// Package 0 contributes its license date to every appended app. That
		// date is unrelated to library inclusion and breaks "recently added".
		// Preserve real package dates; 0 means unknown if discovery has not run.
		pInfo->purchaseTime = g_config.libraryDates.get(appId);
	}

	unlockApp(appId, pInfo);

	return true;
}

void Apps::getSubscribedApps(uint32_t* appList, size_t size, uint32_t& count)
{
	const auto& added = g_config.addedAppIds.get();
	if (!size || !appList)
	{
		// Probe call: report how many extra ids we would append so Steam sizes
		// the real buffer to fit them.
		count = count + added.size();
		return;
	}

	// Append our ids, but never write past the caller's buffer.  `count` still
	// advances to the true total so Steam re-queries with a larger buffer when
	// truncated, matching Steam's own GetSubscribedApps contract.  Writing
	// unconditionally overflowed the buffer whenever the real subscribed set
	// plus our ids exceeded `size` — e.g. a game added (hot-reload) between the
	// size probe and this fill — corrupting adjacent client heap.
	for (const auto& appId : added)
	{
		if (count < size)
			appList[count] = appId;
		++count;
	}

	applistRequested = true;
}

bool Apps::shouldDisableCloud(uint32_t appId)
{
	// Managed apps are injected into package 0 so Steam treats them as
	// owned.  Cloud saves still can't sync: Valve's cloud backend validates
	// ownership server-side and rejects the upload with "Access Denied"
	// (visible in cloud_log.txt).  Disable cloud for them explicitly so Steam
	// doesn't attempt the doomed sync and surface a cloud error to the user.
	// Steam's own ownership answer is never consulted: it flips to "not owned"
	// for genuinely owned apps while the client rebuilds its license set.
	return g_config.disableCloud.get() && g_config.isAddedAppId(appId);
}

bool Apps::shouldDisableCDKey(uint32_t appId)
{
	// AdditionalApps are injected as owned, so a launch-time legacy-key
	// request (GettingLegacyKey) hits Valve's backend, which validates
	// ownership server-side and answers AccessDenied (EResult 15) — the
	// launch then fails before Proton is ever spawned (visible as
	// "LaunchApp failed with GettingLegacyKey with 15" in console_log).
	// Suppress the legacy-key requirement for AddedApps so Steam skips
	// that doomed step and proceeds to launch, mirroring how the mask is
	// dropped for cloud.  The game's own activation DRM (EA/Uplay) is a
	// separate layer handled outside this hook.
	//
	// Crucially this must also cover the base app's DLC appids: Steam
	// queries RequiresLegacyCDKey across the whole app+DLC set at launch,
	// and a single owned DLC whose appinfo still carries hadthirdpartycdkey
	// (e.g. Far Cry 4's season-pass DLC 332220-332232/343700/348080/353870)
	// re-arms GettingLegacyKey and fails the base app's launch even after
	// the base app itself is suppressed.
	if (g_config.isAddedAppId(appId) || Apps::isAddedAppDlcId(appId))
	{
		return true;
	}

	CUser* user = getLocalUser();
	if (user == nullptr)
	{
		return false;
	}
	return !user->isSubscribed(appId);
}

bool Apps::shouldDisableUpdates(uint32_t appId)
{
	if (!g_config.disableUpdates.get())
	{
		return false;
	}

	const bool added = g_config.isAddedAppId(appId);
	if (!added)
	{
		CUser* user = getLocalUser();
		if (user == nullptr)
		{
			return false;
		}
		return !user->isSubscribed(appId);
	}

	// For AdditionalApps we want to suppress UPDATES (so Steam doesn't
	// re-fetch and overwrite the staged build) — but NOT suppress the
	// initial INSTALL.  Returning false from GetUpdateInfo
	// unconditionally made Steam think a not-yet-installed AddedApp had
	// "nothing to download", so the install hung in "Reconfiguring"
	// and got Suspended.  Only disable updates once the app is already
	// fully installed; while it's uninstalled / update-required, let
	// the real update info through so the download proceeds.
	if (g_pClientAppManager != nullptr)
	{
		const EAppState state = g_pClientAppManager->getAppInstallState(appId);
		if (!(state & k_EAppStateFullyInstalled))
		{
			return false;  // allow the install/download to start
		}
	}

	// Locked apps freeze on their pinned build.  But suppressing updates
	// UNCONDITIONALLY means Steam never installs the pinned build in the first
	// place (it stays on whatever is installed); allowing them unconditionally
	// makes Steam loop forever reconciling the pinned depot gid against
	// appinfo's public gid (commit pinned -> "Update Required" -> re-plan ->
	// commit -> ...).  Resolve both: suppress IFF the app's installed depots
	// already match its pins.  Not-yet-pinned -> allow the ONE downgrade to
	// run; once installed==pinned -> suppress so it freezes without looping.
	if (!g_config.isAppLocked(appId))
	{
		if (ManifestFetch::areProvidersOffline())
		{
			return true;  // providers offline: suppress updates
		}
		if (!g_config.autoUpdateApps.get())
		{
			// Global "auto-update apps" is off: freeze this unpinned app on
			// whatever build is installed (mirrors LuaTools' toggle).  Safe:
			// we are past the FULLY_INSTALLED gate so the initial install
			// already ran, and with no pin there is no gid rewrite -> no
			// reconcile loop.  A per-app pin takes the locked branch below
			// and is unaffected by this switch.
			return true;
		}
		return false;  // unlocked AddedApp: updates enabled (grab latest)
	}

	const bool atPinned = appAtPinnedGids(appId);
	g_pLog->infoOnce("Pin-lock %u: installed%s at pinned build -> updates %s\n",
	                 appId, atPinned ? "" : " NOT",
	                 atPinned ? "frozen" : "allowed (apply pin)");
	return atPinned;
}

void Apps::sendAndRecvLastPlayedTimes(const char* name, CPlayer_GetLastPlayedTimes_Response* recv)
{
	if (strcmp(name, "Player.ClientGetLastPlayedTimes#1") != 0)
	{
		return;
	}

	const auto apps = g_config.addedAppIds.get();
	for (int i = recv->games_size() - 1; i >= 0; i--)
	{
		auto game = recv->mutable_games(i);
		if (!apps.contains(game->appid()))
		{
			continue;
		}

		LOG_DEBUG("Removed serverside PlayTime for %u\n", game->appid());
		recv->mutable_games()->DeleteSubrange(i, 1);
	}
}

void Apps::sendGamesPlayed(CNetPacket* pkt)
{
	auto msg = pkt->deserializeBody<CMsgClientGamesPlayed>();

	auto titles = g_config.gameTitles.get();

	for (int i = 0; i < msg.games_played_size(); i++)
	{
		auto game = msg.mutable_games_played(i);
		if (!game->game_id())
		{
			continue;
		}

		const uint64_t gameId = game->game_id();

		// Leave a non-Steam shortcut's title and 64-bit ID untouched.
		if (gameId & GAME_TYPE_SHORTCUT)
		{
			LOG_DEBUG("Preserving non-Steam shortcut %llu\n", gameId);
			continue;
		}

		if (g_config.disableFamilyLock.get())
		{
			game->set_owner_id(1);
		}

		if (titles.contains(gameId))
		{
			game->set_game_extra_info(titles[gameId]);
		}
		//This probably belongs into FakeAppIds, but the GameTitles does not so it stays here
		else if (FakeAppIds::getFakeAppId(gameId))
		{
			char name[256] {}; //No clue how long titles can get
			int len = 0;

			if (privateApps.contains(static_cast<AppId_t>(gameId)))
			{
				strcpy(name, "Redacted");
				len = strlen(name);
			}
			else if (g_pClientApps)
			{
				len = g_pClientApps->getAppData(gameId, "common/name", name, sizeof(name));
			}

			if (len > 0)
			{
				LOG_DEBUG("AppName %s (%i)\n", name, len);
				game->set_game_extra_info(name);
			}
		}

		//msg->mutable_games_played(i)->ParseFromString(game.SerializeAsString());

		LOG_DEBUG("Playing game %llu with flags %u & pid %u\n", gameId, game->game_flags(), game->process_id());
	}

	if (msg.games_played_size() < 1)
	{
		const auto statusApp = g_config.idleStatus.get();
		if (statusApp.appId)
		{
			auto game = msg.add_games_played();
			game->set_game_id(statusApp.appId);
			game->set_game_extra_info(statusApp.title);
			game->set_game_flags(0);

			if (g_config.disableFamilyLock.get())
			{
				game->set_owner_id(1);
			}
			//game->set_game_flags(k_EGameFlagMultiplayer);
		}
	}

	pkt->serialize(msg);
}

void Apps::sendPICSInfoRequest(CNetPacket* pkt)
{
	const auto tokens = g_config.appTokens.get();
	auto msg = pkt->deserializeBody<CMsgClientPICSProductInfoRequest>();

	// Strip locally authoritative AddedApps from Steam's outgoing product-info
	// request. Their account access token may be denied even when our anonymous
	// CM fetch returned complete metadata; the resulting empty refresh clobbers
	// the startup splice. Protection is gated off after full installation and
	// deliberately has no time/count limit.
	{
		std::vector<uint32_t> requested;
		requested.reserve(static_cast<size_t>(msg.apps_size()));
		for (int i = 0; i < msg.apps_size(); ++i)
			requested.push_back(msg.apps(i).appid());

		const auto strip = SynthMark::stripIndices(
		    requested, [](uint32_t appId) { return permitSynthStrip(appId); });

		for (int idx : strip) // descending, safe for in-place delete
		{
			g_pLog->debug("PICS-request: stripping locally authoritative app %u\n",
			              msg.apps(idx).appid());
			msg.mutable_apps()->DeleteSubrange(idx, 1);
		}
	}

	// We intentionally do NOT add AdditionalApps to Steam's outgoing PICS
	// product-info requests.  This mirrors the upstream LumaCore design:
	// ownership is established purely by the package-0 AppIdVec injection in
	// PackagePatch plus the CheckAppOwnership patch, and Steam fetches the
	// product info for those apps through its own normal request/response
	// handshake.  Injecting appids here (or forcing meta_data_only=false)
	// makes Steam follow up forever for buffers it never asked for, which
	// hangs the client at "Loading user data".  We only attach an access
	// token to apps Steam is ALREADY asking about, so the CM returns a real
	// product-info buffer for them.
	for (int i = 0; i < msg.apps_size(); i++)
	{
		auto app = msg.mutable_apps(i);
		if (tokens.contains(app->appid()))
		{
			app->set_access_token(tokens.at(app->appid()));
			g_pLog->debug("PICS-request: attached access token for %u\n", app->appid());
		}
	}

	std::stringstream sentIds;
	for (int i = 0; i < msg.apps_size(); ++i)
	{
		if (i) sentIds << ',';
		sentIds << msg.apps(i).appid();
	}
	g_pLog->debug("PICS-request: apps=%d packages=%d ids=[%s]\n",
	              msg.apps_size(), msg.packages_size(), sentIds.str().c_str());
	pkt->serialize(msg);
}

void Apps::sendMsg(CNetPacket *pkt)
{
	switch(pkt->getProtoBufType())
	{
		case k_EMsgClientPICSProductInfoRequest:
			sendPICSInfoRequest(pkt);
			break;

		case k_EMsgClientGamesPlayed:
		case k_EMsgClientGamesPlayedNoDataBlob:
		case k_EMsgClientGamesPlayedWithDataBlob:
			sendGamesPlayed(pkt);
			break;
		default:
			break;
	}
}

void Apps::setConfigStoreString(const char* key, const char* value)
{
	if (!key || !value || !std::string_view(key).starts_with("WebStorage\\PrivateApps"))
	{
		return;
	}

	LOG_DEBUG("%s -> %s\n", key, value);

	// Steam stores the list as a JSON array ([730,240,440]). Anything that is
	// not a plain app id is skipped: this runs inside a Steam caller, so a
	// throw would unwind into the client.
	std::string_view list(value);
	if (list.starts_with('['))
	{
		list.remove_prefix(1);
	}
	if (list.ends_with(']'))
	{
		list.remove_suffix(1);
	}

	std::unordered_set<AppId_t> apps;
	while (!list.empty())
	{
		const auto comma = list.find(',');
		const auto item = list.substr(0, comma);
		list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);

		AppId_t appId = 0;
		const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), appId);
		if (ec != std::errc() || end != item.data() + item.size() || !appId)
		{
			g_pLog->debug("%.*s is not an app id! Skipping\n", static_cast<int>(item.size()), item.data());
			continue;
		}

		apps.emplace(appId);
		g_pLog->debug("Added %u to privateApps\n", appId);
	}

	privateApps.set(std::move(apps));
}
