// Aggregation policy for "incomplete install data" notifications.
//
// A bulk copy into config/stplug-in routinely contains hundreds of
// storefront-only, DLC, demo or delisted entries that carry no content depots.
// Raising one notification per app buried the useful information under ~133
// identical popups on the 3k-script run, so a pass now reports the first app by
// AppID and aggregates the remainder into a single summary.
//
// Build:
//   g++ -std=c++20 -I include -I src tools/test_provision_notice.cpp -o /tmp/t && /tmp/t

#include "../src/feats/provision_network.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using AppInfoProvision::ProvisionPassState;

	// No failures at all: nothing to report.
	{
		ProvisionPassState pass;
		CHECK(pass.preparationFailureCount() == 0,
		      "a clean pass counts no preparation failures");
		CHECK(pass.pendingPreparationSummary() == 0,
		      "a clean pass emits no aggregated summary");
	}

	// The ordinary case: the user added one game. It must still be named.
	{
		ProvisionPassState pass;
		CHECK(pass.takePreparationNotice(1031840),
		      "a single failing app is reported individually");
		CHECK(pass.firstPreparationFailure() == 1031840,
		      "the individual notice keeps its AppID");
		CHECK(pass.preparationFailureCount() == 1,
		      "a single failure is counted");
		CHECK(pass.pendingPreparationSummary() == 0,
		      "one failure needs no aggregated summary");
	}

	// A bulk copy: only the first app pops, the rest are summarized once.
	{
		ProvisionPassState pass;
		CHECK(pass.takePreparationNotice(1013160),
		      "the first app of a burst is reported individually");
		int individual = 0;
		for (uint32_t appId = 2; appId <= 494; ++appId)
			if (pass.takePreparationNotice(1000000 + appId)) ++individual;
		CHECK(individual == 0,
		      "no further app raises its own notification");
		CHECK(pass.preparationFailureCount() == 494,
		      "every failing app is still counted for the summary");
		CHECK(pass.pendingPreparationSummary() == 493,
		      "the summary accounts for every suppressed notification");
		CHECK(pass.firstPreparationFailure() == 1013160,
		      "the first offender remains identifiable after aggregation");
	}

	// Aggregation must not disturb the pre-existing connectivity policy.
	{
		ProvisionPassState pass;
		CHECK(pass.takeConnectivityNotice(),
		      "connectivity still notifies once per pass");
		CHECK(!pass.takeConnectivityNotice(),
		      "connectivity never notifies twice in one pass");
		CHECK(pass.takePreparationNotice(10),
		      "preparation notices are independent of connectivity");
	}

	if (g_failures == 0) std::printf("\nall provision-notice checks passed\n");
	else                 std::printf("\n%d provision-notice check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
