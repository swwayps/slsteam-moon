// Hooks::setup() must fail, without patching anything, when a required hook
// has no target or its target cannot be patched; load() then leaves Steam
// unmodified instead of reporting success with the hook missing. TraceIPC
// only logs, so it is not required.
// Build: make test-hook-placement (links the whole object set).
#include "../src/config.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/patterns.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/protobufs/steammessages_base.pb.h"
#include "../src/vftableinfo.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

// Once armed, the next allocation on this thread fails.
static thread_local bool t_failNextAllocation = false;
static thread_local bool t_allocationFailed = false;
void* operator new(std::size_t size)
{
	if (t_failNextAllocation)
	{
		t_failNextAllocation = false;
		t_allocationFailed = true;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{

// True when `scenario` returns true in a child process; a crash counts as false.
template <typename Scenario>
bool inChild(Scenario scenario)
{
	std::fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0) std::_Exit(scenario() ? 0 : 1);
	int status = 0;
	return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0;
}

void pointRequiredHooksAt(lm_address_t target, lm_address_t stride = 0)
{
	for (Pattern_t* pattern : {&Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame,
	                           &Patterns::CJobMgr::BRouteMsgToJob,
	                           &Patterns::CDepotDownloadMgr::BYldRequestDepotManifest,
	                           &Patterns::CUser::CheckAppOwnership,
	                           &Patterns::CUser::GetSubscribedApps,
	                           &Patterns::CUser::PostCallbackToAppId,
	                           &Patterns::CSteamEngine::ProcessIPCFrame,
	                           &Patterns::CSteamEngine::SetAppIdForCurrentPipe})
	{
		pattern->address = target;
		target += stride;
	}
	VFTIndexes::CCMInterface::RecvPkt.address = target;
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_hook_placement_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	CHECK(inChild([] { return !Hooks::setup(); }),
	      "with no target resolved, Hooks::setup() fails without placing hooks");

	// An executable page whose bytes do not decode, so no detour can patch it.
	void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) return 2;
	std::memset(page, 0xFF, 4096);
	if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) return 2;
	CHECK(inChild([page] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(page));
		      const bool failed = !Hooks::setup();
		      const auto* bytes = static_cast<const unsigned char*>(page);
		      return failed && bytes[0] == 0xFF && bytes[4] == 0xFF;
	      }),
	      "a required hook that cannot be patched fails Hooks::setup()");

	// Small functions every detour can patch, one per required hook.
	void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED) return 2;
	const unsigned char stub[] = {0x55, 0x89, 0xE5, 0x83, 0xEC, 0x10, 0x90, 0x90, 0xC9, 0xC3};
	for (std::size_t at = 0; at + sizeof(stub) <= 4096; at += 64)
		std::memcpy(static_cast<unsigned char*>(code) + at, stub, sizeof(stub));
	if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) return 2;
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      return Hooks::setup();
	      }),
	      "an unresolved TraceIPC does not fail Hooks::setup()");

	// The CM receive hook carries the Family Share choke and every CM message
	// feature, so it is required like the others.
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      VFTIndexes::CCMInterface::RecvPkt.address = 0;
		      return !Hooks::setup();
	      }),
	      "an unresolved CM receive hook fails Hooks::setup()");
	CHECK(inChild([code, page] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      VFTIndexes::CCMInterface::RecvPkt.address = reinterpret_cast<lm_address_t>(page);
		      return !Hooks::setup();
	      }),
	      "a CM receive hook that cannot be patched fails Hooks::setup()");
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      const auto* bytes = static_cast<const unsigned char*>(code);
		      const std::vector<unsigned char> before(bytes, bytes + 4096);
		      if (!Hooks::setup()) return false;
		      const bool patched = std::memcmp(bytes, before.data(), 4096) != 0;
		      Hooks::remove();
		      return patched && std::memcmp(bytes, before.data(), 4096) == 0;
	      }),
	      "Hooks::remove() restores every hook Hooks::setup() placed");

	// The CM receive hook's own work before the dispatch boundary (packet
	// layout, logging, the Family Share decision) can run out of memory on
	// Steam's receive thread: the hook still hands the packet to Steam.
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      if (!Hooks::setup()) return false;
		      g_config.disableFamilyLock.set(true);
		      CMsgProtoBufHeader header;
		      header.set_target_job_name("FamilyGroupsClient.NotifyRunningApps#1");
		      const std::string bytes = header.SerializeAsString();
		      std::vector<unsigned char> storage(sizeof(CNetPacketBody) + bytes.size());
		      auto* body = reinterpret_cast<CNetPacketBody*>(storage.data());
		      body->type = CNetPacket::PROTOBUF_TYPE_MASK | 146;
		      body->headerSize = bytes.size();
		      std::memcpy(storage.data() + sizeof(CNetPacketBody), bytes.data(), bytes.size());
		      CNetPacket packet{};
		      packet.setBody(body);
		      packet.setSize(storage.size());
		      using RecvPkt = void (*)(void*, CNetPacket*);
		      const auto recv = reinterpret_cast<RecvPkt>(
		          VFTIndexes::CCMInterface::RecvPkt.address);
		      t_failNextAllocation = true;
		      recv(nullptr, &packet);
		      t_failNextAllocation = false;
		      return t_allocationFailed;
	      }),
	      "an allocation failure before the CM dispatch does not unwind into Steam");

	if (g_failures == 0) std::printf("\nall hook placement checks passed\n");
	else std::printf("\n%d hook placement check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
