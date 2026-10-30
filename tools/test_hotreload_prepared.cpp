// A base prepared while another game's config reload is between publishing
// the managed set and its HotReload publication is still published, and that
// reload keeps it ready.
// Build: make test-hotreload-prepared (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
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
	char home[] = "/tmp/test_hotreload_prepared_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	// Bases managed at initialize() start ready, so 42 is added afterwards.
	HotReload::initialize();
	g_config.managedAppIds.set({42});
	HotReload::publish({42});
	// The next reload added 43 to the config but has not published it yet.
	g_config.managedAppIds.set({42, 43});
	const auto token = AppInfoProvision::snapshotCachePublication(42);
	(void)HotReload::publishPreparedBase(42, token.generation);

	// That reload then publishes its own set.
	HotReload::publish({42, 43});

	std::stringstream log;
	log << std::ifstream(logPath).rdbuf();
	CHECK(log.str().find("managed set changed during preparation") == std::string::npos,
	      "a reload in flight does not reject the prepared base");
	CHECK(log.str().find("published prepared base=42") != std::string::npos,
	      "the prepared base is published");
	CHECK(log.str().find("(managed=2 added=1 removed=0 planner_apps=1 ") != std::string::npos,
	      "the reload in flight keeps the base ready");
	if (g_failures != 0) std::printf("--- log ---\n%s", log.str().c_str());
	if (g_failures == 0) std::printf("\nall prepared base checks passed\n");
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
