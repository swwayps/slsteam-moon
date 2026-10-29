// The job-routing hook reads a header length out of a buffer Steam owns. On a
// client whose message layout moved that word is garbage, so a length that
// would wrap the 32-bit bounds check must send the message through untouched
// instead of letting the parser read past the buffer.
// Build: make test-route-msg-bounds (links the whole object set).
#include "../src/feats/manifestcode.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/sdk/protobufs/enums_clientserver.pb.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
int g_routed = 0;
bool countRoute(void*, void*, void*, void*)
{
	++g_routed;
	return true;
}
void put32(uint8_t* at, uint32_t value) { std::memcpy(at, &value, sizeof(value)); }
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	Hooks::CJobMgr_BRouteMsgToJob.tramp.fn = countRoute;
	// A 64-byte buffer that ends where an inaccessible page begins.
	const long page = sysconf(_SC_PAGESIZE);
	auto* pages = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
	                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (pages == MAP_FAILED || mprotect(pages + page, page, PROT_NONE) != 0) return 2;
	uint8_t* buffer = pages + page - 64;
	put32(buffer, k_EMsgServiceMethodResponse | 0x80000000u);
	put32(buffer + 4, 0xFFFFFFF9u);  // header length that wraps 8 + length
	// A header string field that claims more bytes than the buffer holds.
	buffer[8] = 0x62;
	buffer[9] = 0x7F;
	std::memset(buffer + 10, 'x', 54);
	alignas(8) uint8_t inner[16] = {};
	put32(inner + 4, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer)));
	put32(inner + 8, 64);
	alignas(8) uint8_t message[16] = {};
	put32(message + 0xc, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(inner)));
	alignas(8) uint8_t job[32] = {};
	put32(job + 0x10, k_EMsgServiceMethodResponse);

	std::fflush(stdout);
	const pid_t child = fork();
	if (child == 0)
	{
		ManifestCode::hkBRouteMsgToJob(nullptr, nullptr, message, job);
		std::_Exit(g_routed == 1 ? 0 : 1);
	}
	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "a header length that wraps the bounds check routes the message unchanged");
	if (g_failures == 0) std::printf("\nall route message checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
