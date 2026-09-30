// LaunchOptions resolution (Apps::spawnGame, called from the CUser::SpawnGameId
// detour). Links the real function; stubs config membership and licenses.
// Build and run: make test-launch-options
#include "../src/config.hpp"
#include "../src/feats/apps.hpp"
#include "../src/feats/stats_policy.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

std::unordered_set<uint32_t> managed;
std::unordered_set<uint32_t> licensed;

struct Launch
{
	std::string cmd;
	std::string cmdline;
};

Launch spawn(uint64_t gameId)
{
	Launch launch { "/game/bin", "/game/bin -flag" };
	Apps::spawnGame(&gameId, launch.cmd, launch.cmdline);
	return launch;
}
}

CConfig g_config;
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CConfig::~CConfig() = default;
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
bool CConfig::isAddedAppId(uint32_t appId) { return managed.contains(appId); }
bool StatsPolicy::hasNativeLicense(uint32_t app) { return licensed.contains(app); }

int main()
{
	managed = { 100, 200 };
	licensed = { 200 };

	g_config.launchOptions = std::unordered_map<AppId_t, std::string> {};
	Launch launch = spawn(100);
	check(launch.cmd == "/game/bin" && launch.cmdline == "/game/bin -flag",
	      "no option leaves the launch alone");

	g_config.launchOptions = std::unordered_map<AppId_t, std::string> {
		{ 100, "gamemoderun %command% -extra" },
	};
	launch = spawn(100);
	check(launch.cmdline == "gamemoderun /game/bin -flag -extra" && launch.cmd == "gamemoderun",
	      "%command% is replaced and the first word becomes the executable");

	g_config.launchOptions = std::unordered_map<AppId_t, std::string> {
		{ 100, "/other/bin -x" },
	};
	launch = spawn(100);
	check(launch.cmdline == "/other/bin -x" && launch.cmd == "/other/bin",
	      "an option without %command% replaces the command line");

	g_config.launchOptions = std::unordered_map<AppId_t, std::string> {
		{ UINT32_MAX - 1, "managed %command%" },
		{ UINT32_MAX, "all %command%" },
	};
	check(spawn(100).cmd == "managed", "the managed key applies to a managed app without its own license");
	check(spawn(200).cmd == "all", "a managed app with its own license takes the all-apps key");
	check(spawn(300).cmd == "all", "an app outside the managed set takes the all-apps key");

	check(spawn(100 | 0x2000000ULL).cmd == "/game/bin", "shortcuts are never rewritten");

	std::string cmd = "/game/bin";
	std::string cmdline = "/game/bin -flag";
	Apps::spawnGame(nullptr, cmd, cmdline);
	check(cmd == "/game/bin" && cmdline == "/game/bin -flag", "a missing game id is ignored");

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
