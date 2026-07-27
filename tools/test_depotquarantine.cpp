// Standalone test for the in-memory DLC quarantine policy.
//
// Build:
//   g++ -std=c++20 tools/test_depotquarantine.cpp \
//       -o /tmp/test_depotquarantine && /tmp/test_depotquarantine

#include "../src/feats/depotquarantine.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	DepotQuarantine::Registry quarantine;
	const std::string badKey(32, '\x11');
	const std::string replacementKey(32, '\x22');

	CHECK(DepotQuarantine::isTargetFailure(4, DepotQuarantine::kUnpackFailed),
	      "downloaded chunk with unpack result 4 is the target failure");
	CHECK(!DepotQuarantine::isTargetFailure(2, DepotQuarantine::kUnpackFailed),
	      "local chunk-store failure is ignored");
	CHECK(!DepotQuarantine::isTargetFailure(4, 3),
	      "other downloaded-chunk failures are ignored");

	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey, 3, 0x101),
	      "non-unpack failures are ignored");
	CHECK(!quarantine.shouldDrop(2473120, 2473120, true, badKey),
	      "ignored failure never drops a depot");
	CHECK(!quarantine.markFailure(2473120, 1902690, false, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x101),
	      "out-of-scope depots cannot enter quarantine");
	CHECK(!quarantine.markFailure(2473120, 1902690, true, "short",
	                              DepotQuarantine::kUnpackFailed, 0x101),
	      "invalid key snapshots cannot enter quarantine");

	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x101),
	      "one failed chunk is only a candidate");
	CHECK(!quarantine.contains(2473120),
	      "candidate depot stays outside the retry fast path");
	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x101),
	      "repeating the same chunk does not advance the threshold");
	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x202),
	      "two distinct failed chunks are still insufficient");
	CHECK(quarantine.markFailure(2473120, 1902690, true, badKey,
	                             DepotQuarantine::kUnpackFailed, 0x303),
	      "three distinct unpack failures quarantine the depot once");
	CHECK(quarantine.contains(2473120),
	      "quarantine lookup identifies the confirmed depot");
	CHECK(!quarantine.contains(1902696),
	      "quarantine lookup leaves unrelated depots on the fast path");
	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x404),
	      "additional failed chunks do not create repeated events");

	CHECK(!quarantine.shouldDrop(2473120, 0, true, badKey),
	      "base depot is never dropped even when its id is quarantined");
	CHECK(quarantine.shouldDrop(2473120, 2473120, true, badKey),
	      "DLC depot with the same failed key is dropped on retry");
	CHECK(!quarantine.shouldDrop(1902696, 2473120, true, badKey),
	      "quarantine is scoped to the failing depot id");

	CHECK(!quarantine.shouldDrop(2473120, 2473120, true, replacementKey),
	      "a replacement key automatically releases quarantine");
	CHECK(!quarantine.contains(2473120),
	      "released depot leaves the retry lookup");
	CHECK(!quarantine.shouldDrop(2473120, 2473120, true, badKey),
	      "released quarantine does not revive when an old key reappears");

	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x501),
	      "new failure series starts below threshold");
	CHECK(!quarantine.markFailure(2473120, 1902690, true, badKey,
	                              DepotQuarantine::kUnpackFailed, 0x502),
	      "new failure series still requires another distinct chunk");
	CHECK(quarantine.markFailure(2473120, 1902690, true, badKey,
	                             DepotQuarantine::kUnpackFailed, 0x503),
	      "later confirmed series can quarantine the depot again");
	CHECK(!quarantine.shouldDrop(2473120, 2473120, false, badKey),
	      "a depot that leaves managed scope is released");

	// A quarantine restored from disk must take effect on the FIRST plan of
	// the next session, without waiting for the failures to happen again.
	{
		DepotQuarantine::Registry restored;
		CHECK(!restored.contains(2473121),
		      "a fresh registry starts with no quarantines");
		CHECK(restored.adopt(2473121, 1902690, badKey),
		      "a persisted quarantine is adopted at startup");
		CHECK(restored.contains(2473121),
		      "an adopted depot is on the retry fast path immediately");
		CHECK(restored.shouldDrop(2473121, 2473121, true, badKey),
		      "an adopted DLC depot is dropped without new failures");
		CHECK(!restored.shouldDrop(2473121, 0, true, badKey),
		      "adoption never drops base/shared content");
		CHECK(!restored.adopt(2473121, 1902690, badKey),
		      "adopting the same depot twice reports no new quarantine");
		CHECK(!restored.adopt(0, 1902690, badKey),
		      "adoption requires a depot id");
		CHECK(!restored.adopt(2494230, 0, badKey),
		      "adoption requires an app id");
		CHECK(!restored.adopt(2494230, 1902690, "short"),
		      "adoption requires a well-formed key");
		CHECK(!restored.contains(2494230),
		      "rejected adoptions leave the depot untouched");
		CHECK(!restored.shouldDrop(2473121, 2473121, true, replacementKey),
		      "a replaced key releases an adopted quarantine too");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
