#include "fakeappid.hpp"

#include "../config.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CSteamMatchmakingServers.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/IClientUtils.hpp"

uint32_t FakeAppIds::lastAppLaunched;

std::unordered_map<uint32_t, uint32_t> FakeAppIds::fakeAppIdMap = std::unordered_map<uint32_t, uint32_t>();
std::unordered_map<uint32_t, uint32_t> FakeAppIds::fakeAppIdMapServer = std::unordered_map<uint32_t, uint32_t>();
std::unordered_map<uint64_t, uint32_t> FakeAppIds::fakeAppIdMapPings = std::unordered_map<uint64_t, uint32_t>();

uint32_t FakeAppIds::getFakeAppId(uint32_t appId)
{
	auto fakeAppIds = g_config.fakeAppIds.get();

	if (fakeAppIds.contains(appId))
	{
		return fakeAppIds[appId];
	}
	else if (fakeAppIds.contains(0))
	{
		CUser* user = getLocalUser();
		if (user != nullptr && !user->isSubscribed(appId))
		{
			return fakeAppIds[0];
		}
	}

	return 0;
}

uint32_t FakeAppIds::getRealAppIdForCurrentPipe(bool fallback)
{
	// g_pClientUtils is populated lazily from IClientUtils::RunIPCFrame.
	// Under the LD_PRELOAD injection model our hooks are placed after
	// Steam is already running, so callers (ticket recv, DLC, matchmaking)
	// can reach here before that hook has fired.  Guard against the null
	// pointer instead of dereferencing it.
	if (!g_pClientUtils)
	{
		return 0;
	}

	uint32_t hPipe = *g_pClientUtils->getPipeIndex();
	if (fakeAppIdMap.contains(hPipe))
	{
		return fakeAppIdMap[hPipe];
	}

	if (fallback)
	{
		return g_pClientUtils->getAppId();
	}

	return 0;
}

bool FakeAppIds::shouldUseRealAppIdForInterface(const EInterfaceType type)
{
	switch(type)
	{
		//case k_EInterfaceTypeClientUser:
		//case k_EInterfaceTypeClientGameServerInternal:
		//case k_EInterfaceTypeClientFriends:
		case k_EInterfaceTypeClientUtils:
		case k_EInterfaceTypeClientBilling:
		//case k_EInterfaceTypeClientMatchmaking:
		case k_EInterfaceTypeClientApps:
		case k_EInterfaceTypeClientUserStats:
		//case k_EInterfaceTypeClientNetworking:
		case k_EInterfaceTypeClientRemoteStorage:
		case k_EInterfaceTypeClientDepotBuilder:
		case k_EInterfaceTypeClientAppManager:
		case k_EInterfaceTypeClientConfigStore:
		//case k_EInterfaceTypeClientGameCoordinator:
		//case k_EInterfaceTypeClientGameServerStats:
		case k_EInterfaceTypeClientGameStats:
		case k_EInterfaceTypeClientHTTP:
		case k_EInterfaceTypeClientScreenshots:
		case k_EInterfaceTypeClientAudio:
		case k_EInterfaceTypeClientUnifiedMessages:
		case k_EInterfaceTypeClientStreamLauncher:
		case k_EInterfaceTypeClientParentalSettings:
		case k_EInterfaceTypeClientNetworkDeviceManager:
		case k_EInterfaceTypeClientMusic:
		case k_EInterfaceTypeClientRemoteClientManager:
		case k_EInterfaceTypeClientUGC:
		case k_EInterfaceTypeClientStreamClient:
		case k_EInterfaceTypeClientProductBuilder:
		case k_EInterfaceTypeClientShortcuts:
		case k_EInterfaceTypeClientGameNotifications:
		case k_EInterfaceTypeClientVideo:
		case k_EInterfaceTypeClientInventory:
		case k_EInterfaceTypeClientVR:
		case k_EInterfaceTypeClientControllerSerialized:
		case k_EInterfaceTypeClientAppDisableUpdate:
		case k_EInterfaceTypeClientSharedConnection:
		case k_EInterfaceTypeClientShader:
		//case k_EInterfaceTypeClientNetworkingSocketsSerialized:
		case k_EInterfaceTypeClientCompat:
		case k_EInterfaceTypeClientParties:
		//case k_EInterfaceTypeClientNetworkingUtilsSerialized:
		case k_EInterfaceTypeClientRemotePlay:
		//case k_EInterfaceTypeClientGameServerPacketHandler:
		case k_EInterfaceTypeClientSystemManager:
		case k_EInterfaceTypeClientSystemPerfManager:
		case k_EInterfaceTypeClientSystemDockManager:
		case k_EInterfaceTypeClientSystemAudioManager:
		case k_EInterfaceTypeClientSystemDisplayManager:
		case k_EInterfaceTypeClientTimeline:
			return true;

		default:
			return false;
	}
}

void FakeAppIds::launchApp(const AppId_t appId)
{
	lastAppLaunched = appId;
}

void FakeAppIds::setAppIdForCurrentPipe(uint32_t& appId)
{
	if (!g_pClientUtils)
	{
		return;
	}

	//Keep track of every AppId, for various reasons
	//fakeAppIdMap[*g_pClientUtils->getPipeIndex()] = appId;
	fakeAppIdMap[*g_pClientUtils->getPipeIndex()] = lastAppLaunched;

	g_pLog->debug("fakeAppIdMap[%p] = %u\n", *g_pClientUtils->getPipeIndex(), appId);

	//Do not change Steam Client itself (AppId 0)
	if (!appId)
	{
		return;
	}

	uint32_t newAppId = getFakeAppId(appId);
	if (newAppId)
	{
		g_pLog->infoOnce("Changing AppId of %u\n", appId);
		appId = newAppId;
	}
}

void FakeAppIds::runIPCFrame(const bool post, const EInterfaceType interface)
{
	if (!shouldUseRealAppIdForInterface(interface))
	{
		return;
	}

	AppId_t appId = getRealAppIdForCurrentPipe(false);
	const AppId_t fakeAppId = getFakeAppId(appId);

	if (!appId || !fakeAppId || appId == fakeAppId)
	{
		return;
	}

	if (post)
	{
		appId = fakeAppId;
	}

	if (!g_pClientUtils)
	{
		return;
	}
	g_pLog->debug("Setting AppId to %u in pipe %p\n", appId, *g_pClientUtils->getPipeIndex());

	if (!g_pSteamEngine)
	{
		return;
	}
	g_pSteamEngine->setAppIdForCurrentPipe(appId);
}

void FakeAppIds::getServerDetails(uint32_t handle, gameserverdetails_t& details)
{
	if (!fakeAppIdMapServer.contains(handle))
	{
		return;
	}

	const uint32_t realAppId = fakeAppIdMapServer[handle];

	fakeAppIdMapPings[*reinterpret_cast<uint64_t*>(&details.address)] = realAppId;
	details.appId = realAppId;

	g_pLog->debug("Changing appId back to %u\n", realAppId);
}

uint32_t FakeAppIds::requestInternetServerList(uint32_t appId)
{
	const uint32_t fake = getFakeAppId(appId);
	if (!fake)
	{
		return 0;
	}

	g_pLog->debug("Replacing %u with %u\n", appId, fake);
	return fake;
}

void FakeAppIds::pingResponse(gameserverdetails_t *details)
{
	if (!details)
	{
		return;
	}

	const uint64_t ip = *reinterpret_cast<uint64_t*>(&details->address);
	if (!fakeAppIdMapPings.contains(ip))
	{
		return;
	}

	details->appId = fakeAppIdMapPings[ip];
}

void FakeAppIds::sendGamesPlayed(CNetPacket* packet)
{
	auto message = packet->deserializeBody<CMsgClientGamesPlayed>();
	for (int i = 0; i < message.games_played_size(); i++)
	{
		const auto game = message.mutable_games_played(i);
		const uint64_t gameId = game->game_id();

		// Preserve native non-Steam shortcut IDs instead of applying a fake AppID.
		if ((gameId & 0xffffffffULL) == 0x02000000ULL)
		{
			continue;
		}

		const uint32_t fakeAppId = FakeAppIds::getFakeAppId(gameId);
		if (!fakeAppId)
		{
			continue;
		}

		g_pLog->debug("Setting %llu to %u\n", gameId, fakeAppId);
		game->set_game_id(fakeAppId);
	}

	packet->serialize(message);
}

void FakeAppIds::sendRichPresenceUpload(CNetPacket* packet)
{
	CMsgProtoBufHeader header;
	if (!packet->deserializeHeader(header))
	{
		return;
	}
	g_pLog->debug("Routing appId %u\n", header.routing_appid());

	const auto appId = getFakeAppId(header.routing_appid());

	if (!appId)
	{
		return;
	}

	//This won't fix localized rich presences, but it's better than nothing
	header.set_routing_appid(appId);
	const auto message = packet->deserializeBody<CMsgClientRichPresenceUpload>();
	packet->serialize(message, &header);
}

void FakeAppIds::sendMsg(CNetPacket* packet)
{
	switch(packet->getProtoBufType())
	{
		case EMSG_GAMESPLAYED:
		case EMSG_GAMESPLAYED_NO_DATABLOB:
		case EMSG_GAMESPLAYED_WITH_DATABLOB:
			sendGamesPlayed(packet);
			break;

		case EMSG_RICH_PRESENCE_UPLOAD:
			sendRichPresenceUpload(packet);
			break;

		default:
			break;
	}
}
