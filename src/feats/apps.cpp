#include "apps.hpp"

#include "../sdk/CAppOwnershipInfo.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EReleaseState.hpp"
#include "../sdk/IClientApps.hpp"
#include "../sdk/IClientAppManager.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "appinfo_provision.hpp"
#include "fakeappid.hpp"
#include "synthmark.hpp"

#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

bool Apps::applistRequested;
std::map<uint32_t, int> Apps::appIdOwnerOverride;

bool Apps::unlockApp(uint32_t appId, CAppOwnershipInfo* info, uint32_t ownerId)
{
	info->owner = ownerId;
	info->realOwner = 0;
	info->familyShared = ownerId != g_currentSteamId;

	info->licensePermanent = !info->familyShared;
	info->retailLicense = false;
	info->licenseExpired = false;
	info->licensePending = false;
	info->licenseLocked = false;

	info->releaseState = ERELEASESTATE_RELEASED;
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

bool Apps::unlockApp(uint32_t appId, CAppOwnershipInfo* info)
{
	return unlockApp(appId, info, g_currentSteamId);
}

bool Apps::checkAppOwnership(uint32_t appId, CAppOwnershipInfo* pInfo)
{
	if (!applistRequested || !pInfo || !g_currentSteamId)
	{
		return false;
	}

	const uint32_t denuvoOwner = g_config.getDenuvoGameOwner(appId);

	if (denuvoOwner && denuvoOwner != g_currentSteamId)
	{
		g_pLog->infoOnce("Skipping %u because it's a Denuvo game from someone else\n", appId);
		return false;
	}

	if (g_config.shouldExcludeAppId(appId))
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

	const bool manualUnlock = g_config.isAddedAppId(appId);
	if (!manualUnlock && (!g_config.playNotOwnedGames.get() || pInfo->ownsLicense))
	{
		return false;
	}

	if (!manualUnlock && g_config.automaticFilter.get())
	{
		if (!g_pClientApps)
		{
			return false;
		}

		auto type = g_pClientApps->getAppType(appId);
		if (type == APPTYPE_DLC) //Don't touch DLC here, otherwise downloads might break. Hopefully this won't decrease compatibility
		{
			return false;
		}

		switch(type)
		{
			case APPTYPE_APPLICATION:
			case APPTYPE_GAME:
				break;

			default:
				return false;
		}
	}

	unlockApp(appId, pInfo);

	return true;
}

void Apps::getSubscribedApps(uint32_t* appList, size_t size, uint32_t& count)
{
	if (!size || !appList)
	{
		count = count + g_config.addedAppIds.get().size();
		return;
	}

	for(auto& appId : g_config.addedAppIds.get())
	{
		appList[count++] = appId;
	}

	applistRequested = true;
}

namespace
{
	// DLC appids belonging to AdditionalApps, registered once from setup()
	// (main.cpp) after collectDlcAppIdsForAddedApps().  Read from Steam
	// worker threads via shouldDisableCDKey, so guard with a mutex.  Set
	// once before any app launch; the lock is uncontended in practice.
	std::mutex g_addedAppDlcMutex;
	std::unordered_set<uint32_t> g_addedAppDlcIds;
}

void Apps::setAddedAppDlcIds(const std::vector<uint32_t>& dlcIds)
{
	std::lock_guard<std::mutex> lk(g_addedAppDlcMutex);
	g_addedAppDlcIds.clear();
	g_addedAppDlcIds.insert(dlcIds.begin(), dlcIds.end());
}

bool Apps::isAddedAppDlcId(uint32_t appId)
{
	std::lock_guard<std::mutex> lk(g_addedAppDlcMutex);
	return g_addedAppDlcIds.count(appId) != 0;
}

bool Apps::shouldDisableCloud(uint32_t appId)
{
	if (!g_config.disableCloud.get())
	{
		return false;
	}

	// AdditionalApps are injected into package 0 so Steam treats them as
	// owned — which means isSubscribed() returns true for them.  Cloud
	// saves still can't sync: Valve's cloud backend validates ownership
	// server-side and rejects the upload with "Access Denied" (visible in
	// cloud_log.txt).  Disable cloud for AddedApps explicitly so Steam
	// doesn't attempt the doomed sync and surface a cloud error to the
	// user; the isSubscribed() check below would otherwise be defeated by
	// our own ownership injection.
	if (g_config.isAddedAppId(appId))
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
		if (!(state & APPSTATE_FULLY_INSTALLED))
		{
			return false;  // allow the install/download to start
		}
	}
	return true;
}

void Apps::sendGamesPlayed(CMsgClientGamesPlayed* msg)
{
	auto titles = g_config.gameTitles.get();
	bool owned = false;

	for(int i = 0; i < msg->games_played_size(); i++)
	{
		auto game = CMsgClientGamesPlayed_GamePlayed(msg->games_played(i));

		if (!game.game_id())
		{
			continue;
		}

		if(!owned)
		{
			CUser* user = getLocalUser();
			if (user != nullptr && user->isSubscribed(game.game_id()))
			{
				owned = true;
			}
		}

		if (g_config.disableFamilyLock.get())
		{
			game.set_owner_id(1);
		}

		if (titles.contains(game.game_id()))
		{
			game.set_game_extra_info(titles[game.game_id()]);
		}
		else if (!owned || FakeAppIds::getFakeAppId(game.game_id()))
		{
			char name[256] {}; //No clue how long titles can get
			if (g_pClientApps)
			{
				g_pClientApps->getAppData(game.game_id(), "common/name", name, sizeof(name));
				g_pLog->debug("AppName %s\n", name);
				game.set_game_extra_info(name);
			}
		}

		msg->mutable_games_played(i)->ParseFromString(game.SerializeAsString());

		g_pLog->debug("Playing game %llu with flags %u & pid %u\n", game.game_id(), game.game_flags(), game.process_id());
	}

	if (owned || msg->games_played_size() > 0)
	{
		return;
	}

	const auto statusApp = g_config.idleStatus.get();
	if (statusApp.appId)
	{
		auto game = msg->add_games_played();
		game->set_game_id(statusApp.appId);
		game->set_game_extra_info(statusApp.title);
		game->set_game_flags(0);

		if (g_config.disableFamilyLock.get())
		{
			game->set_owner_id(1);
		}
	}
}

void Apps::sendPICSInfoRequest(CMsgClientPICSProductInfoRequest* msg)
{
	const auto tokens = g_config.appTokens.get();

	// Strip token-locked synthetic AddedApps from Steam's outgoing
	// product-info request.  These apps' access token is DENIED, so Steam's
	// refresh response is an EMPTY buffer; letting it through clobbers the
	// depots + installdir we synthesized into appinfo at startup, dropping
	// the install dialog to 0 B with "Invalid install path".  By removing
	// them from the request, Steam never re-fetches them and keeps the
	// startup splice.  (Removing — unlike ADDING, HANDOFF dead-end — does not
	// make Steam chase buffers it never asked for.)
	{
		std::vector<uint32_t> requested;
		requested.reserve(static_cast<size_t>(msg->apps_size()));
		for (int i = 0; i < msg->apps_size(); ++i)
			requested.push_back(msg->apps(i).appid());

		const auto strip = SynthMark::stripIndices(
		    requested,
		    [](uint32_t a) { return AppInfoProvision::isSynthesizedApp(a); });

		for (int idx : strip) // descending, safe for in-place delete
		{
			g_pLog->debug("PICS-request: stripping token-locked synthetic app %u\n",
			              msg->apps(idx).appid());
			msg->mutable_apps()->DeleteSubrange(idx, 1);
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
	for (int i = 0; i < msg->apps_size(); i++)
	{
		auto app = msg->mutable_apps(i);
		if (tokens.contains(app->appid()))
		{
			app->set_access_token(tokens.at(app->appid()));
			g_pLog->debug("PICS-request: attached access token for %u\n", app->appid());
		}
	}

	std::stringstream sentIds;
	for (int i = 0; i < msg->apps_size(); ++i)
	{
		if (i) sentIds << ',';
		sentIds << msg->apps(i).appid();
	}
	g_pLog->debug("PICS-request: apps=%d packages=%d ids=[%s]\n",
	              msg->apps_size(), msg->packages_size(), sentIds.str().c_str());
}

void Apps::sendMsg(CProtoBufMsgBase *msg)
{
	switch(msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_REQUEST:
			sendPICSInfoRequest(msg->getBody<CMsgClientPICSProductInfoRequest>());
			break;

		case EMSG_GAMESPLAYED:
		case EMSG_GAMESPLAYED_NO_DATABLOB:
		case EMSG_GAMESPLAYED_WITH_DATABLOB:
			sendGamesPlayed(msg->getBody<CMsgClientGamesPlayed>());
			break;
	}
}
