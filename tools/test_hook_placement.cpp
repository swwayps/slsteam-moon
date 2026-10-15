// Hooks::setup() must fail, without patching anything, when a required hook
// has no target or its target cannot be patched; load() then leaves Steam
// unmodified instead of reporting success with the hook missing.
// Build: make test-hook-placement (links the whole object set).
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/patterns.hpp"
#include "../src/vftableinfo.hpp"

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

void pointRequiredHooksAt(lm_address_t target)
{
	for (Pattern_t* pattern : {&Patterns::TraceIPC,
	                           &Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame,
	                           &Patterns::CJobMgr::BRouteMsgToJob,
	                           &Patterns::CDepotDownloadMgr::BYldRequestDepotManifest,
	                           &Patterns::CUser::CheckAppOwnership,
	                           &Patterns::CUser::GetSubscribedApps,
	                           &Patterns::CUser::PostCallbackToAppId,
	                           &Patterns::CSteamEngine::ProcessIPCFrame,
	                           &Patterns::CSteamEngine::SetAppIdForCurrentPipe})
		pattern->address = target;
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

	if (g_failures == 0) std::printf("\nall hook placement checks passed\n");
	else std::printf("\n%d hook placement check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
