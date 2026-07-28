// Standalone test for Apps::cloudDisableDecision.
//
// Cloud must be reported disabled for an app whose ownership WE fabricate:
// Valve validates ownership server-side and answers Access Denied, so the sync
// is doomed and only produces a cloud error for the user.
//
// It must NOT be reported disabled merely because Steam currently answers "not
// owned" for an arbitrary app. That answer is unreliable while the client is
// rebuilding its license set (a rebuild our own package-0 reconcile triggers),
// and acting on it silently turns cloud saves off for a genuinely owned game
// for the whole session.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_clouddecision.cpp -o /tmp/test_clouddecision && /tmp/test_clouddecision

#include "../src/feats/clouddecision.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	// The config switch is authoritative.
	CHECK(Apps::cloudDisableDecision(false, true, false, false) == false,
	      "config off keeps cloud enabled even for a managed app");

	// Apps we unlock ourselves: the sync is doomed, so disable it.
	CHECK(Apps::cloudDisableDecision(true, true, false, false) == true,
	      "managed app disables cloud");
	CHECK(Apps::cloudDisableDecision(true, true, false, true) == true,
	      "managed app disables cloud even when Steam reports it owned");

	// The regression: an owned app queried while the license set is being
	// rebuilt reads as not owned, and we must not act on that.
	CHECK(Apps::cloudDisableDecision(true, false, false, false) == false,
	      "unmanaged app keeps cloud enabled when we unlock nothing globally");
	CHECK(Apps::cloudDisableDecision(true, false, false, true) == false,
	      "owned unmanaged app keeps cloud enabled");

	// With global unlocking on, Steam's verdict is the only signal that an app
	// is playable through our ownership hook.
	CHECK(Apps::cloudDisableDecision(true, false, true, false) == true,
	      "globally unlocked app disables cloud when Steam reports it unowned");
	CHECK(Apps::cloudDisableDecision(true, false, true, true) == false,
	      "owned app keeps cloud enabled with global unlocking on");

	std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n",
	            g_failures);
	return g_failures == 0 ? 0 : 1;
}
