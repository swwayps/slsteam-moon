#include "fakeappid.hpp"

#include "../config.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CSteamMatchmakingServers.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/IClientUtils.hpp"


GameId_t FakeAppIds::lastLaunchedGame = 0;
std::unordered_map<HSteamPipe, AppId_t> FakeAppIds::fakeAppIdMap = std::unordered_map<HSteamPipe, AppId_t>();
std::unordered_map<HServerRequest, AppId_t> FakeAppIds::fakeAppIdMapServer = std::unordered_map<HServerRequest, AppId_t>();
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

	if (fakeAppIdMap.contains(utils->getCurrentSteamPipe()))
	{
		return fakeAppIdMap.at(utils->getCurrentSteamPipe());
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

void FakeAppIds::closePipe(const HSteamPipe pipe)
{
	if (fakeAppIdMap.contains(pipe))
	{
		fakeAppIdMap.erase(pipe);
	}
}

void FakeAppIds::launchApp(const GameId_t gameId)
{
	lastLaunchedGame = gameId;
}

void FakeAppIds::setAppIdForCurrentPipe(AppId_t& appId)
{
	//Do not fill fakeAppIdMap with non-steam games
	if (lastLaunchedGame & GAME_TYPE_SHORTCUT)
	{
		return;
	}

	const auto utils = g_pSteamEngine ? g_pSteamEngine->getUtils() : nullptr;
	if (utils)
	{
		fakeAppIdMap[utils->getCurrentSteamPipe()] = static_cast<AppId_t>(lastLaunchedGame);
	}

	//Do not change Steam Client itself (AppId 0)
	if (!appId)
	{
		return;
	}

	uint32_t newAppId = getFakeAppId(appId);
	if (newAppId)
	{
		g_pLog->debugOnce("Changing AppId of %u\n", appId);
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

	if (g_config.extendedLogging.get())
	{
		const auto utils = g_pSteamEngine->getUtils();
		g_pLog->debug("Setting AppId to %u in pipe %u\n", appId, utils ? utils->getCurrentSteamPipe() : 0);
	}

	g_pSteamEngine->setAppIdForCurrentPipe(appId);
}

void FakeAppIds::getServerDetails(const HServerRequest handle, gameserverdetails_t& details)
{
	if (!fakeAppIdMapServer.contains(handle))
	{
		return;
	}

	const AppId_t realAppId = fakeAppIdMapServer.at(handle);
	fakeAppIdMapPings[details.ip64] = realAppId;
	details.appId = realAppId;

	LOG_DEBUG("Changing appId back to %u\n", realAppId);
}

uint32_t FakeAppIds::requestInternetServerList(uint32_t appId)
{
	const uint32_t fake = getFakeAppId(appId);
	if (!fake)
	{
		return 0;
	}

	LOG_DEBUG("Replacing %u with %u\n", appId, fake);
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

	details->appId = fakeAppIdMapPings.at(ip);
}

void FakeAppIds::sendGamesPlayed(CNetPacket* packet)
{
	auto message = packet->deserializeBody<CMsgClientGamesPlayed>();
	for (int i = 0; i < message.games_played_size(); i++)
	{
		const auto game = message.mutable_games_played(i);
		const uint64_t gameId = game->game_id();

		if (gameId & GAME_TYPE_SHORTCUT)
		{
			continue;
		}

		const uint32_t fakeAppId = FakeAppIds::getFakeAppId(gameId);
		if (!fakeAppId)
		{
			continue;
		}

		LOG_DEBUG("Setting %lu to %u\n", gameId, fakeAppId);
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
		case k_EMsgClientGamesPlayed:
		case k_EMsgClientGamesPlayedNoDataBlob:
		case k_EMsgClientGamesPlayedWithDataBlob:
			sendGamesPlayed(packet);
			break;

		case k_EMsgClientRichPresenceUpload:
			sendRichPresenceUpload(packet);
			break;

		default:
			break;
	}
}
