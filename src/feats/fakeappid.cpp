#include "fakeappid.hpp"

#include "../config.hpp"
#include "../process.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CSteamMatchmakingServers.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/IClientUtils.hpp"

std::unordered_map<HSteamPipe, AppId_t> FakeAppIds::fakeAppIdMap = std::unordered_map<HSteamPipe, AppId_t>();
std::unordered_map<uint32_t, AppId_t> FakeAppIds::fakeAppIdMapServer = std::unordered_map<uint32_t, AppId_t>();
std::unordered_map<uint64_t, AppId_t> FakeAppIds::fakeAppIdMapPings = std::unordered_map<uint64_t, AppId_t>();

AppId_t FakeAppIds::getFakeAppId(const AppId_t appId)
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

AppId_t FakeAppIds::getRealAppIdFromEnv(const HSteamPipe pipe)
{
	if (g_processMap.contains(pipe))
	{
		return g_processMap.at(pipe).appId;
	}
	return 0;
}

AppId_t FakeAppIds::getRealAppIdForCurrentPipe(const bool fallback)
{
	if (!g_pSteamEngine)
	{
		return 0;
	}
	const auto utils = g_pSteamEngine->getUtils();
	if (!utils)
	{
		return 0;
	}

	const AppId_t appId = getRealAppIdFromEnv(utils->getCurrentSteamPipe());
	if (appId)
	{
		return appId;
	}

	if (fallback)
	{
		return utils->getAppId();
	}

	return 0;
}

bool FakeAppIds::shouldUseRealAppIdForInterface(const EIPCInterface type)
{
	switch(type)
	{
		//case EIPCInterface::User:
		//case EIPCInterface::GameServerInternal:
		//case EIPCInterface::Friends:
		case EIPCInterface::Utils:
		case EIPCInterface::Billing:
		//case EIPCInterface::Matchmaking:
		case EIPCInterface::Apps:
		case EIPCInterface::UserStats:
		//case EIPCInterface::Networking:
		case EIPCInterface::RemoteStorage:
		case EIPCInterface::DepotBuilder:
		case EIPCInterface::AppManager:
		case EIPCInterface::ConfigStore:
		//case EIPCInterface::GameCoordinator:
		//case EIPCInterface::GameServerStats:
		case EIPCInterface::GameStats:
		case EIPCInterface::HTTP:
		case EIPCInterface::Screenshots:
		case EIPCInterface::Audio:
		case EIPCInterface::UnifiedMessages:
		case EIPCInterface::StreamLauncher:
		case EIPCInterface::ParentalSettings:
		case EIPCInterface::NetworkDeviceManager:
		case EIPCInterface::Music:
		case EIPCInterface::RemoteClientManager:
		case EIPCInterface::UGC:
		case EIPCInterface::StreamClient:
		case EIPCInterface::ProductBuilder:
		case EIPCInterface::Shortcuts:
		case EIPCInterface::GameNotifications:
		case EIPCInterface::Video:
		case EIPCInterface::Inventory:
		case EIPCInterface::VR:
		case EIPCInterface::ControllerSerialized:
		case EIPCInterface::AppDisableUpdate:
		case EIPCInterface::SharedConnection:
		case EIPCInterface::Shader:
		//case EIPCInterface::NetworkingSocketsSerialized:
		case EIPCInterface::Compat:
		case EIPCInterface::Parties:
		//case EIPCInterface::NetworkingUtilsSerialized:
		case EIPCInterface::RemotePlay:
		//case EIPCInterface::GameServerPacketHandler:
		case EIPCInterface::SystemManager:
		case EIPCInterface::SystemPerfManager:
		case EIPCInterface::SystemDockManager:
		case EIPCInterface::SystemAudioManager:
		case EIPCInterface::SystemDisplayManager:
		case EIPCInterface::Timeline:
			return true;

		default:
			return false;
	}
}

void FakeAppIds::setAppIdForCurrentPipe(AppId_t& appId)
{
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

void FakeAppIds::runIPCFrame(const bool post, const EIPCInterface interface)
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

	const auto utils = g_pSteamEngine->getUtils();
	if (!utils)
	{
		return;
	}
	if (g_config.extendedLogging.get())
	{
		g_pLog->debug("Setting AppId to %u in pipe %u\n", appId, utils->getCurrentSteamPipe());
	}
	g_pSteamEngine->setAppIdForCurrentPipe(appId);
}

void FakeAppIds::getServerDetails(uint32_t handle, gameserverdetails_t& details)
{
	if (!fakeAppIdMapServer.contains(handle))
	{
		return;
	}

	const AppId_t realAppId = fakeAppIdMapServer[handle];
	fakeAppIdMapPings[details.ip64] = realAppId;
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

	const uint64_t ip = details->ip64;
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

		if (gameId & 0x2000000ULL)
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
