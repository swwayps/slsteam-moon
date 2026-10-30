// The donor sends through the CM connection it captured from Steam's own
// authenticated frames. Steam destroys that object when the CM socket drops
// (its vtable pointer then reads the CNetConnection base), well before the
// next logon replaces the capture, so a send must check the object first.
// Build: make test-donor-connection (links the whole object set).
#include "../src/feats/manifestcode.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/protobufs/enums_clientserver.pb.h"
#include "../src/sdk/protobufs/steammessages_base.pb.h"
#include "../src/sdk/steam.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
namespace
{
const int kLiveVtable = 1;
const int kBaseVtable = 2;
struct FakeConnection { const void* vptr; };
int g_sendsTo = 0;
FakeConnection* g_connection = nullptr;
bool recordSend(void* connection, EWebSocketOpCode, uint8_t*, uint32_t)
{
	if (connection == g_connection) ++g_sendsTo;
	return true;
}
void* plainAlloc(int size) { return std::malloc(static_cast<std::size_t>(size)); }
// A ServiceMethod call that carries the logged-on identity, as Steam sends.
std::vector<uint8_t> authenticatedFrame()
{
	CMsgProtoBufHeader header;
	header.set_steamid(76561197960287930ULL);
	header.set_client_sessionid(1234);
	header.set_target_job_name("Player.ClientGetLastPlayedTimes#1");
	const std::string bytes = header.SerializeAsString();
	std::vector<uint8_t> frame(sizeof(MsgHdr) + bytes.size());
	MsgHdr prefix{k_EMsgServiceMethodCallFromClient | kMsgHdrProtoFlag,
	              static_cast<uint32_t>(bytes.size())};
	std::memcpy(frame.data(), &prefix, sizeof(prefix));
	std::memcpy(frame.data() + sizeof(prefix), bytes.data(), bytes.size());
	return frame;
}
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	Steam::Plat_Alloc = plainAlloc;
	Steam::Plat_Free = std::free;
	Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn = recordSend;
	// A received packet on the classic layout settles the field shift.
	alignas(8) uint8_t body[64] = {};
	alignas(8) uint8_t received[0x40] = {};
	const void* bodyPtr = body;
	const uint32_t size = sizeof(body);
	std::memcpy(received + CNetPacket::OFF_BODY, &bodyPtr, sizeof(bodyPtr));
	std::memcpy(received + CNetPacket::OFF_SIZE, &size, sizeof(size));
	CNetPacket::detectLayout(reinterpret_cast<const CNetPacket*>(received));

	FakeConnection connection{&kLiveVtable};
	g_connection = &connection;
	auto frame = authenticatedFrame();
	ManifestCode::hkBBuildAndAsyncSendFrame(
		&connection, k_eWebSocketOpCode_Binary, frame.data(),
		static_cast<uint32_t>(frame.size()));
	g_sendsTo = 0;
	(void)ManifestCode::requestCode(480, 481, 1);
	CHECK(g_sendsTo == 1, "a request goes out through the captured connection");

	connection.vptr = &kBaseVtable;  // what the destructor leaves behind
	g_sendsTo = 0;
	(void)ManifestCode::requestCode(480, 481, 2);
	CHECK(g_sendsTo == 0, "a destroyed connection is not used for a request");
	return g_failures == 0 ? 0 : 1;
}
