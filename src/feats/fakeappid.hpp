#pragma once

#include "../sdk/steam.hpp"

#include <cstdint>
#include <unordered_map>

class CNetPacket;

struct gameserverdetails_t;
struct servernetadr_t;

namespace FakeAppIds
{
	extern std::unordered_map<HSteamPipe, AppId_t> fakeAppIdMap;
	extern std::unordered_map<uint32_t, AppId_t> fakeAppIdMapServer;
	extern std::unordered_map<uint64_t, AppId_t> fakeAppIdMapPings;

	AppId_t getFakeAppId(const AppId_t appId);
	AppId_t getRealAppIdFromEnv(const HSteamPipe pipe);
	AppId_t getRealAppIdForCurrentPipe(const bool fallback = true);
	bool shouldUseRealAppIdForInterface(const EIPCInterface type);

	//General functionality
	void setAppIdForCurrentPipe(AppId_t& appId);
	void runIPCFrame(const bool post, const EIPCInterface interface);

	//Serverbrowser
	void getServerDetails(uint32_t handle, gameserverdetails_t& details);
	uint32_t requestInternetServerList(uint32_t appId);
	void pingResponse(gameserverdetails_t* details);

	void sendGamesPlayed(CNetPacket* packet);
	void sendRichPresenceUpload(CNetPacket* packet);
	void sendMsg(CNetPacket* packet);
}
