// Regression test for FakeAppIds rich-presence routing.
//
// Build (from repo root):
//   make test-fakeappid-presence

// Steam's game-played message was already rewritten to the configured fake
// AppID, but ClientRichPresenceUpload (EMsg 7501) was not. That split leaves
// lobby rich presence under the real AppID while both friends appear to play
// the fake one, so Steam cannot expose the matching invite/join action.

#include "../src/config.hpp"
#include "../src/feats/fakeappid.hpp"
#include "../src/sdk/CProtoBufMsgBase.hpp"
#include "../src/sdk/CUser.hpp"

#include <cstdio>
#include <memory>
#include <unordered_map>

// Keep this standalone test linked only to the production FakeAppIds module.
// These are the narrow runtime globals that module reads; Steam interfaces are
// deliberately absent because the explicit-map path must not need them.
CConfig g_config;
std::unique_ptr<CLog> g_pLog;

CConfig::~CConfig() = default;
CLog::CLog(const char* logPath) : path(logPath) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
bool CLog::shouldNotify() { return false; }
CUser* getLocalUser() { return nullptr; }
bool CUser::isSubscribed(uint32_t) { return false; }

namespace
{
constexpr uint16_t kClientRichPresenceUpload = 7501;

int failures = 0;

#define CHECK(condition, message)                                           \
	do {                                                                     \
		if (!(condition)) { std::printf("FAIL: %s\n", message); ++failures; } \
		else              { std::printf("ok:   %s\n", message); }            \
	} while (0)

void route(uint32_t realAppId, uint32_t fakeAppId, uint32_t expected)
{
	g_config.fakeAppIds = std::unordered_map<uint32_t, uint32_t>{
		{realAppId, fakeAppId},
	};

	CMsgProtoBufHeader header;
	header.set_routing_appid(realAppId);
	CMsgClientRichPresenceUpload body;

	CProtoBufMsgBase message{};
	message.type = kClientRichPresenceUpload;
	message.header = &header;
	message.__pBody = &body;

	FakeAppIds::sendMsg(&message);
	CHECK(header.routing_appid() == expected,
	      "rich presence uses the configured routing AppID");
}
}

int main()
{
	g_config.watcher = nullptr;
	g_pLog = std::make_unique<CLog>("/dev/null");

	route(4001890, 480, 480);

	g_config.fakeAppIds = std::unordered_map<uint32_t, uint32_t>{};
	CMsgProtoBufHeader header;
	header.set_routing_appid(123);
	CMsgClientRichPresenceUpload body;
	CProtoBufMsgBase message{};
	message.type = kClientRichPresenceUpload;
	message.header = &header;
	message.__pBody = &body;
	FakeAppIds::sendMsg(&message);
	CHECK(header.routing_appid() == 123,
	      "rich presence without a mapping keeps its original route");

	return failures == 0 ? 0 : 1;
}
