// ManifestIds and DepotBlacklist are applied in CUserAppManager::
// BuildDepotDependency; ManifestPins are staged and planned later by the
// download manager. A depot named by both must keep its planned gid here, so
// only the pin pipeline decides which build it gets.
//
// Links the real Apps::buildDepotDependency; stubs only config and logging.
// Build: `make test-depot-overrides`.
#include "../src/config.hpp"
#include "../src/feats/apps.hpp"
#include "../src/sdk/CUtl.hpp"
#include "../src/sdk/IClientAppManager.hpp"

#include <cstdio>
#include <memory>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}
}

CConfig g_config;
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CConfig::~CConfig() = default;
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
uint64_t CConfig::getManifestPinForPlanner(uint32_t appId, uint32_t depotId)
{
	return ManifestPins::getPinForPlannerEntry(manifestPinsByApp.get(), appId, depotId);
}

int main()
{
	constexpr uint32_t app = 1621690;
	constexpr uint32_t pinnedDepot = 1621691;
	constexpr uint32_t freeDepot = 1621692;

	DepotInfo_t storage[2] {};
	storage[0].depotId = pinnedDepot;
	storage[0].appId = app;
	storage[0].manifestId = 100;
	storage[1].depotId = freeDepot;
	storage[1].appId = app;
	storage[1].manifestId = 200;

	CUtlVector<DepotInfo_t> depots {};
	depots.memory.base = storage;
	depots.memory.alloc = 2;
	depots.size = 2;
	CUtlVector<DepotInfo_t> shared {};

	ManifestPins::PinMap pins;
	pins[app].depots[pinnedDepot] = 111;
	g_config.manifestPinsByApp.set(pins);
	g_config.manifestIds.set({{pinnedDepot, 999}, {freeDepot, 222}});

	Apps::buildDepotDependency(app, &depots, &shared);
	check(storage[0].manifestId == 100,
	      "a depot pinned by ManifestPins keeps its planned gid");
	check(storage[1].manifestId == 222,
	      "ManifestIds still overrides a depot without a pin");

	// DepotBlacklist removes by moving the last entry into the freed slot;
	// that entry must be checked too. Planned: A (listed), B, C (listed).
	constexpr uint32_t listedA = 1621693;
	constexpr uint32_t keptB = 1621694;
	constexpr uint32_t listedC = 1621695;
	DepotInfo_t plan[3] {};
	plan[0].depotId = listedA;
	plan[1].depotId = keptB;
	plan[2].depotId = listedC;
	for (auto& entry : plan) entry.appId = app;
	CUtlVector<DepotInfo_t> planned {};
	planned.memory.base = plan;
	planned.memory.alloc = 3;
	planned.size = 3;
	g_config.manifestPinsByApp.set({});
	g_config.manifestIds.set({});
	g_config.depotBlacklist.set({listedA, listedC});

	Apps::buildDepotDependency(app, &planned, &shared);
	check(planned.size == 1 && plan[0].depotId == keptB,
	      "every blacklisted depot leaves the plan, including the one moved into a freed slot");

	check(!planned.swap(0, planned.size), "swap refuses an index one past the end");

	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
