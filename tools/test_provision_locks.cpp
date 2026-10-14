// The PICS callbacks run on Steam's CM receive thread. What they ask the
// provisioner must not wait for a provisioning pass, which holds its mutex
// across fleet-wide disk work.
// Build: make test-provision-locks (links the whole object set).
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

// Runs `call` on its own thread; true when it returned within two seconds.
template <typename Call>
bool returnsPromptly(Call call)
{
	const auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread([done, call] {
		call();
		done->store(true);
	}).detach();
	for (int i = 0; i < 200 && !done->load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return done->load();
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_provision_locks_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	HotReload::initialize();
	HotReload::publish({42}, true);

	// A provisioning pass is running.
	AppInfoProvision::provisioningPassMutex().lock();
	CHECK(returnsPromptly([] { (void)AppInfoProvision::snapshotCachePublication(42); }),
	      "a publication snapshot does not wait for a provisioning pass");
	CHECK(returnsPromptly([] { (void)AppInfoProvision::observeTerminal(42, 1); }),
	      "a terminal lookup does not wait for a provisioning pass");
	AppInfoProvision::provisioningPassMutex().unlock();

	// A HotReload publication holds its coordinator while it waits here.
	AppInfoProvision::cachePublicationMutex().lock();
	std::thread([] { HotReload::publish({42, 43}, true); }).detach();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	CHECK(returnsPromptly([] { (void)HotReload::takeMissingCacheRepairRequests(); }),
	      "cache repair scheduling does not wait for a HotReload publication");
	CHECK(returnsPromptly([] { HotReload::repairMissingDlcMetadata("/nonexistent/appinfo.vdf"); }),
	      "DLC metadata repair scheduling does not wait for a HotReload publication");

	if (g_failures == 0) std::printf("\nall provision lock checks passed\n");
	else std::printf("\n%d provision lock check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	// A blocked call still holds its thread, so skip static destructors.
	std::_Exit(g_failures == 0 ? 0 : 1);
}
