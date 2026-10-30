// The outgoing-frame hook leaves frames alone until the packet field layout
// has been detected: the detection on the receive thread can change the field
// offsets while this hook is still building its scratch packet.
// Build: make test-send-frame (links the whole object set).
#include "../src/feats/manifestcode.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/sdk/protobufs/steammessages_base.pb.h"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/steam.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
// Counts down allocations on this thread; the one that reaches zero fails.
static thread_local int t_failIn = 0;
static thread_local bool t_failed = false;
void* operator new(std::size_t size)
{
	if (t_failIn > 0 && --t_failIn == 0)
	{
		t_failed = true;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
namespace
{
int g_allocations = 0;
const uint8_t* g_sentData = nullptr;
void* countingAlloc(int size)
{
	++g_allocations;
	return std::malloc(static_cast<std::size_t>(size));
}
int g_sends = 0;
bool recordSend(void*, EWebSocketOpCode, uint8_t* data, uint32_t)
{
	++g_sends;
	g_sentData = data;
	return true;
}
int g_manifestRequests = 0;
bool recordManifestRequest(void*, uint32_t, uint32_t, uint64_t, const char*, void*)
{
	++g_manifestRequests;
	return true;
}
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	Steam::Plat_Alloc = countingAlloc;
	Steam::Plat_Free = std::free;
	Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn = recordSend;

	uint8_t frame[32] = {};
	ManifestCode::hkBBuildAndAsyncSendFrame(nullptr, k_eWebSocketOpCode_Binary, frame, sizeof(frame));
	CHECK(g_allocations == 0 && g_sentData == frame,
	      "before the layout is known a frame is sent as it came");

	// A packet layout neither shift knows: the body word points into an
	// unmapped page while the size looks sane. Detection must not read it.
	{
		const long page = sysconf(_SC_PAGESIZE);
		auto* pages = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
		                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		const bool mapped = pages != MAP_FAILED && munmap(pages + page, page) == 0;
		alignas(8) uint8_t unknown[0x40] = {};
		const void* unmapped = pages + page;
		const uint32_t plausible = 64;
		std::memcpy(unknown + CNetPacket::OFF_BODY, &unmapped, sizeof(unmapped));
		std::memcpy(unknown + CNetPacket::OFF_SIZE, &plausible, sizeof(plausible));
		std::fflush(stdout);
		const pid_t child = fork();
		if (child == 0)
		{
			CNetPacket::detectLayout(reinterpret_cast<const CNetPacket*>(unknown));
			std::_Exit(CNetPacket::layoutSettled() ? 1 : 0);
		}
		int status = -1;
		waitpid(child, &status, 0);
		CHECK(mapped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "layout detection skips a body pointer into unmapped memory");
	}

	// A received packet on the classic layout settles the shift at 0.
	alignas(8) uint8_t body[64] = {};
	alignas(8) uint8_t received[0x40] = {};
	const void* bodyPtr = body;
	const uint32_t size = sizeof(body);
	std::memcpy(received + CNetPacket::OFF_BODY, &bodyPtr, sizeof(bodyPtr));
	std::memcpy(received + CNetPacket::OFF_SIZE, &size, sizeof(size));
	CNetPacket::detectLayout(reinterpret_cast<const CNetPacket*>(received));
	ManifestCode::hkBBuildAndAsyncSendFrame(nullptr, k_eWebSocketOpCode_Binary, frame, sizeof(frame));
	CHECK(CNetPacket::layoutSettled() && g_allocations == 1,
	      "once the layout is known a frame goes through the rewrite path");

	// Our work in these hooks runs on Steam's threads: an allocation that
	// fails anywhere in it must not unwind into Steam, and Steam's own call
	// still runs once.
	CMsgProtoBufHeader header;
	header.set_target_job_name("ContentServerDirectory.GetManifestRequestCode#1");
	header.set_steamid(76561198000000001ULL);
	header.set_client_sessionid(7);
	const std::string headerBytes = header.SerializeAsString();
	std::vector<uint8_t> call(8 + headerBytes.size());
	const uint32_t eMsg = 151u | 0x80000000u; // ServiceMethodCallFromClient
	const uint32_t headerLength = static_cast<uint32_t>(headerBytes.size());
	std::memcpy(call.data(), &eMsg, 4);
	std::memcpy(call.data() + 4, &headerLength, 4);
	std::memcpy(call.data() + 8, headerBytes.data(), headerBytes.size());
	int sendRounds = 0;
	bool sendEscaped = false;
	bool sendOnce = true;
	for (int k = 1; k <= 400; ++k)
	{
		g_sends = 0;
		t_failed = false;
		t_failIn = k;
		try
		{
			ManifestCode::hkBBuildAndAsyncSendFrame(
			    nullptr, k_eWebSocketOpCode_Binary, call.data(), static_cast<uint32_t>(call.size()));
		}
		catch (...) { sendEscaped = true; }
		t_failIn = 0;
		if (!t_failed) break;
		++sendRounds;
		sendOnce = sendOnce && g_sends == 1;
	}
	CHECK(sendRounds > 0 && !sendEscaped && sendOnce,
	      "an allocation failure in the send hook does not reach Steam, and the frame is sent once");

	Hooks::CDepotDownloadMgr_BYldRequestDepotManifest.tramp.fn = recordManifestRequest;
	int manifestRounds = 0;
	bool manifestEscaped = false;
	bool manifestOnce = true;
	for (int k = 1; k <= 400; ++k)
	{
		g_manifestRequests = 0;
		t_failed = false;
		t_failIn = k;
		try
		{
			ManifestCode::hkCDepotDownloadMgr_BYldRequestDepotManifest(
			    nullptr, 42, 4201, 7, "", nullptr);
		}
		catch (...) { manifestEscaped = true; }
		t_failIn = 0;
		if (!t_failed) break;
		++manifestRounds;
		manifestOnce = manifestOnce && g_manifestRequests == 1;
	}
	CHECK(manifestRounds > 0 && !manifestEscaped && manifestOnce,
	      "an allocation failure in the manifest request hook does not reach Steam, "
	      "and Steam's request runs once");
	return g_failures == 0 ? 0 : 1;
}
