// Standalone test for the pure install-staging planning logic
// (src/feats/pics.hpp :: PICS::buildSyncStagePlan).
//
// Why this exists
// ---------------
// The install-time PICS recv handler stages every depot-with-key for an
// AdditionalApp SYNCHRONOUSLY before it returns, so the manifests are on
// disk before Steam plans the install (else Steam calls
// BYldRequestDepotManifest -> "Access Denied" -> the ~30s retry).
//
// The original handler did this SEQUENTIALLY (awaitManifestBlob in a
// loop), so a big title (DL2, 36 depots) blocked the Steam worker thread
// for 15s+ and froze the Install-dialog buttons.  The fix is to keep the
// guarantee (all on disk before return) but kick off every fetch first
// and only then await them all, so the wall time is the slowest fetch,
// not the sum.
//
// buildSyncStagePlan is the PURE step that decides WHICH (appId, depotId,
// gid) manifests to stage — deduplicated, key-gated — separated from the
// concurrent I/O so the decision is unit-testable without Steam, libcurl
// or disk.  Parallelizing must NOT change which depots are staged; these
// tests pin that down.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_pics.cpp -o /tmp/test_pics && /tmp/test_pics

#include "../src/feats/pics.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool hasTarget(const std::vector<PICS::StageTarget>& v,
                      uint32_t appId, uint32_t depotId, uint64_t gid)
{
	return std::any_of(v.begin(), v.end(),
	    [&](const PICS::StageTarget& t) {
	        return t.appId == appId && t.depotId == depotId && t.gid == gid;
	    });
}

int main()
{
	// 1) Keeps only depots we hold a key for; the appId is carried through.
	{
		std::vector<PICS::AppDepots> apps{
			{ 367520, { {367521, 1001}, {367522, 1002} } },
		};
		auto hasKey = [](uint32_t depotId) { return depotId == 367521; };
		auto plan = PICS::buildSyncStagePlan(apps, hasKey);
		CHECK(plan.size() == 1, "plan: keeps only keyed depot");
		CHECK(hasTarget(plan, 367520, 367521, 1001), "plan: kept depot carries appId");
		CHECK(!hasTarget(plan, 367520, 367522, 1002), "plan: dropped keyless depot");
	}

	// 2) A gid of 0 (config-only / no public manifest) is never staged.
	{
		std::vector<PICS::AppDepots> apps{
			{ 100, { {200, 0}, {201, 55} } },
		};
		auto allKeys = [](uint32_t) { return true; };
		auto plan = PICS::buildSyncStagePlan(apps, allKeys);
		CHECK(plan.size() == 1 && hasTarget(plan, 100, 201, 55),
		      "plan: drops gid==0, keeps real manifest");
	}

	// 3) The same (depotId, gid) shared across two AddedApps is staged once.
	{
		std::vector<PICS::AppDepots> apps{
			{ 10, { {500, 9000} } },
			{ 11, { {500, 9000} } },
		};
		auto allKeys = [](uint32_t) { return true; };
		auto plan = PICS::buildSyncStagePlan(apps, allKeys);
		CHECK(plan.size() == 1, "plan: dedups identical depot/gid across apps");
		CHECK(hasTarget(plan, 10, 500, 9000),
		      "plan: first app wins the deduped target");
	}

	// 4) Same depotId but a different gid is a different manifest -> both kept.
	{
		std::vector<PICS::AppDepots> apps{
			{ 7, { {300, 1}, {300, 2} } },
		};
		auto allKeys = [](uint32_t) { return true; };
		auto plan = PICS::buildSyncStagePlan(apps, allKeys);
		CHECK(plan.size() == 2, "plan: same depot, different gid both staged");
	}

	// 5) Multiple distinct depots across multiple apps are all kept.
	{
		std::vector<PICS::AppDepots> apps{
			{ 1, { {10, 100}, {11, 101} } },
			{ 2, { {20, 200} } },
		};
		auto allKeys = [](uint32_t) { return true; };
		auto plan = PICS::buildSyncStagePlan(apps, allKeys);
		CHECK(plan.size() == 3, "plan: all distinct depots kept across apps");
		CHECK(hasTarget(plan, 1, 10, 100) && hasTarget(plan, 1, 11, 101) &&
		      hasTarget(plan, 2, 20, 200),
		      "plan: every distinct depot present with its app");
	}

	// 6) Empty inputs are safe.
	{
		auto allKeys = [](uint32_t) { return true; };
		CHECK(PICS::buildSyncStagePlan({}, allKeys).empty(),
		      "plan: no apps -> empty");
		std::vector<PICS::AppDepots> emptyDepots{ { 42, {} } };
		CHECK(PICS::buildSyncStagePlan(emptyDepots, allKeys).empty(),
		      "plan: app with no depots -> empty");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
