#pragma once

#include <cstdint>
#include <unordered_map>

class CProtoBufMsgBase;

struct gameserverdetails_t;
struct servernetadr_t;

namespace FakeAppIds
{
	extern uint32_t lastAppLaunched;

	extern std::unordered_map<uint32_t, uint32_t> fakeAppIdMap;
	extern std::unordered_map<uint32_t, uint32_t> fakeAppIdMapServer;
	extern std::unordered_map<uint64_t, uint32_t> fakeAppIdMapPings;

	uint32_t getFakeAppId(uint32_t appId);
	uint32_t getRealAppIdForCurrentPipe(bool fallback = true);

	//General functionality
	void launchApp(uint32_t appId);
	void setAppIdForCurrentPipe(uint32_t& appId);
	void runIPCFrame(bool post);

	//Serverbrowser
	void getServerDetails(uint32_t handle, gameserverdetails_t& details);
	uint32_t requestInternetServerList(uint32_t appId);
	void pingResponse(gameserverdetails_t* details);

	void sendGamesPlayed(CProtoBufMsgBase* msg);
	void sendRichPresenceUpload(CProtoBufMsgBase* msg);
	void sendMsg(CProtoBufMsgBase* msg);
}
