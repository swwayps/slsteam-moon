// Standalone test for the pure AdditionalApps discovery decision
// (src/config_discovery.hpp).
//
// Regression under test
// ---------------------
// A discovered main-app id must survive even when the same numeric id is
// also a managed depot. Single-depot titles reuse the app id as their own
// content depot id, so the stplug-in script carries a keyed
// `addappid(<appid>, 1, "<key>")` line for the app itself (No Man's Sky
// 275850). Commit 9d062c6 rejected such ids via `!isManagedDepot(id)`,
// hiding the game from Steam's library. Green Hell (815370) keys only its
// sibling depots, so it slipped through and masked the bug.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_config_discovery.cpp -o /tmp/test_config_discovery && /tmp/test_config_discovery

#include "../src/config_discovery.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using ConfigDiscovery::appIdFromScriptName;
	using ConfigDiscovery::keepDiscoveredMainApp;

	// --- Filename -> app id parsing ---------------------------------------
	CHECK(appIdFromScriptName("275850.lua") == 275850,
	      "numeric stem parses to app id");
	CHECK(appIdFromScriptName("815370.lua") == 815370,
	      "another numeric stem parses");
	CHECK(appIdFromScriptName("keys.lua") == 0,
	      "non-numeric stem rejected");
	CHECK(appIdFromScriptName("275850_backup.lua") == 0,
	      "stem with trailing text rejected");
	CHECK(appIdFromScriptName("275850.txt") == 0,
	      "non-.lua extension rejected");
	CHECK(appIdFromScriptName(".lua") == 0,
	      "empty stem rejected");
	CHECK(appIdFromScriptName("275850") == 0,
	      "missing extension rejected");
	CHECK(appIdFromScriptName("99999999999.lua") == 0,
	      "stem overflowing uint32 rejected");

	// --- The core regression ----------------------------------------------
	// No Man's Sky: base appid 275850 is ALSO a keyed depot in its script,
	// so isManagedDepot(275850) is true. It MUST still be kept.
	CHECK(keepDiscoveredMainApp(275850, /*idIsAlsoManagedDepot=*/true),
	      "single-depot title kept even when its appid is a managed depot");

	// Green Hell: base appid is not one of its keyed depots.
	CHECK(keepDiscoveredMainApp(815370, /*idIsAlsoManagedDepot=*/false),
	      "normal title kept");

	// A zero id is never a real app.
	CHECK(!keepDiscoveredMainApp(0, false),
	      "zero id rejected");
	CHECK(!keepDiscoveredMainApp(0, true),
	      "zero id rejected regardless of managed flag");

	if (g_failures == 0) std::printf("\nall config-discovery checks passed\n");
	else                 std::printf("\n%d config-discovery check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
