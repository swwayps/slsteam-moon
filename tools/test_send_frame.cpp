// The outgoing-frame hook leaves frames alone until the packet field layout
// has been detected: the detection on the receive thread can change the field
// offsets while this hook is still building its scratch packet.
// Build: make test-send-frame (links the whole object set).
#include "../src/feats/manifestcode.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/steam.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
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
bool recordSend(void*, EWebSocketOpCode, uint8_t* data, uint32_t)
{
	g_sentData = data;
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
	return g_failures == 0 ? 0 : 1;
}
