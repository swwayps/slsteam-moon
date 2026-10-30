// Startup cache repair: a base with a terminal verdict leaves the rotation,
// and a repair request reuses a pair that has become valid instead of
// fetching it again.
// Build: make test-cache-repair (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/feats/provision_terminal.hpp"
#include "../src/log.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_cache_repair_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	// Neither base has a cache pair at boot; 42 has a terminal verdict.
	const std::string cacheDir = g_config.getDir() + "/cache";
	std::filesystem::create_directories(cacheDir);
	const ProvisionTerminal::Record terminal{
		ProvisionTerminal::kSchema, 42, ProvisionTerminal::Kind::VirtualDlc, 7, "-"};
	if (!ProvisionTerminal::Store(cacheDir).publish(terminal)) return 2;
	g_config.addedAppIds.set({42, 43});
	g_config.managedAppIds.set({42, 43});
	HotReload::initialize();
	HotReload::publish({42, 43});
	const auto repairs = HotReload::takeMissingCacheRepairRequests();
	CHECK(repairs.size() == 1 && repairs.front().appId == 43,
	      "a base with a terminal verdict leaves the startup repair rotation");
	CHECK(repairs.size() == 1 && !repairs.front().forceRefresh &&
	          repairs.front().publishRuntime,
	      "a startup repair reuses a pair that has become valid");
	if (g_failures == 0) std::printf("\nall cache repair checks passed\n");
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
