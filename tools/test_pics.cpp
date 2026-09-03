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
// The handler once created one std::async thread per target, even when the
// exact manifest was already in depotcache.  A title with hundreds of depots
// exhausted the 32-bit Steam process.  The current policy filters ready
// targets to zero jobs and sends only misses to a fixed worker pool.
//
// buildSyncStagePlan is the PURE step that decides WHICH (appId, depotId,
// gid) manifests to stage — deduplicated, key-gated — separated from the
// I/O so the decision is unit-testable without Steam, libcurl or disk.
// buildPendingStagePlan then removes exact manifests already on disk.  These
// tests pin both decisions down without imposing a target-count limit.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_pics.cpp -o /tmp/test_pics && /tmp/test_pics

#include "../src/feats/pics.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <unordered_set>
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
	// Product-info callback work is scoped to managed AppIDs actually present
	// in this response; unrelated managed apps and unmanaged response entries
	// must not leak into synchronous recovery or async refresh planning.
	{
		const std::unordered_set<uint32_t> managed{420530, 4496490, 638510};
		const std::vector<uint32_t> response{4496490, 999999, 4496490};
		CHECK(PICS::selectManagedResponseApps(managed, response) ==
		          std::unordered_set<uint32_t>{4496490},
		      "response scope: selects only managed AppIDs in the current response");
	}
	CHECK(PICS::rawResponseCanRepairCache(
	          true, AppInfoProvision::CacheReadiness::Missing) &&
	      PICS::rawResponseCanRepairCache(
	          true, AppInfoProvision::CacheReadiness::Invalid),
	      "response scope: complete raw response repairs missing or invalid cache");
	CHECK(!PICS::rawResponseCanRepairCache(
	          false, AppInfoProvision::CacheReadiness::Missing) &&
	      !PICS::rawResponseCanRepairCache(
	          true, AppInfoProvision::CacheReadiness::ValidStale),
	      "response scope: empty and ready pairs stay out of raw repair");
	CHECK(PICS::rawResponseCanRepairCache(
	          true, AppInfoProvision::CacheReadiness::Busy) &&
	      PICS::rawResponseCanRepairCache(
	          true, AppInfoProvision::CacheReadiness::Unverified),
	      "response scope: complete raw response avoids network for unverified pairs");
	CHECK(PICS::rawCacheItemShouldReplace(3, 7, 3, 8) &&
	      PICS::rawCacheItemShouldReplace(3, 8, 3, 8) &&
	      !PICS::rawCacheItemShouldReplace(3, 9, 3, 8),
	      "response cache queue: newest change wins within one generation");
	CHECK(PICS::rawCacheItemShouldReplace(3, 99, 4, 1) &&
	      !PICS::rawCacheItemShouldReplace(4, 1, 3, 99),
	      "response cache queue: managed generation dominates change number");
	CHECK(!PICS::rawCacheWorkerShouldContinueAfterDeferral(false) &&
	      PICS::rawCacheWorkerShouldContinueAfterDeferral(true),
	      "response cache queue: a concurrent arrival keeps the worker draining");

	// Runtime publication is a recovery path only for locally authoritative
	// rows whose delivery to Steam was suppressed. Ordinary product-info rows
	// retain the default disk-only refresh behavior.
	{
		const std::vector<AppInfoProvision::RefreshRequest> requests{
			{420530, 8, 3, AppInfoProvision::reasonMask(
				AppInfoProvision::RefreshReason::PicsProductInfo), true, false},
			{4496490, 9, 4, AppInfoProvision::reasonMask(
				AppInfoProvision::RefreshReason::PicsChanges), true, false},
		};
		const auto tagged = PICS::markRuntimePublicationForSuppressedApps(
			requests, std::unordered_set<uint32_t>{4496490});
		CHECK(!tagged[0].publishRuntime && tagged[1].publishRuntime,
		      "runtime publication: only suppressed authoritative response rows are tagged");
		const auto managedSynthetic = PICS::markRuntimePublicationForSuppressedApps(
			requests, std::unordered_set<uint32_t>{123});
		CHECK(!managedSynthetic[0].publishRuntime &&
		      !managedSynthetic[1].publishRuntime,
		      "runtime publication: unrelated changelist rows remain disk-only");
	}

	// A cache normalized by callback-side cold recovery must not immediately
	// be enqueued again by the observation-driven async refresh step.
	{
		const std::vector<AppInfoProvision::RefreshRequest> requests{
			{420530, 8, 3, AppInfoProvision::reasonMask(
				AppInfoProvision::RefreshReason::PicsProductInfo), true, false},
			{4496490, 9, 4, AppInfoProvision::reasonMask(
				AppInfoProvision::RefreshReason::PicsProductInfo), true, false},
		};
		const auto pending = PICS::excludeRefreshRequestsForApps(
			requests, std::unordered_set<uint32_t>{4496490});
		CHECK(pending.size() == 1 && pending.front().appId == 420530,
		      "response scope: normalized cold app is excluded from async requests");
	}

	// 0) The old PICS-wide synchronous staging/prewarm path is rollback-only.
	// Event-driven plan staging is the default and the legacy path requires an
	// explicit exact "1".
	CHECK(!PICS::legacyManifestStagingEnabled(nullptr),
	      "legacy staging: disabled when env is absent");
	CHECK(!PICS::legacyManifestStagingEnabled("0"),
	      "legacy staging: disabled by zero");
	CHECK(!PICS::legacyManifestStagingEnabled("true"),
	      "legacy staging: rejects ambiguous values");
	CHECK(PICS::legacyManifestStagingEnabled("1"),
	      "legacy staging: exact one enables rollback path");

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

	// 7) Manifests already present in depotcache must not be submitted or
	// awaited again.  Only genuinely-missing targets remain in the I/O plan.
	{
		const std::vector<PICS::StageTarget> plan{
			{ 1, 10, 100 },
			{ 1, 11, 101 },
			{ 2, 20, 200 },
		};
		int checks = 0;
		const auto pending = PICS::buildPendingStagePlan(
		    plan,
		    [&](const PICS::StageTarget& target)
		    {
		        ++checks;
		        return target.depotId != 11;
		    });
		CHECK(checks == 3, "pending plan: checks every target exactly once");
		CHECK(pending.size() == 1 && hasTarget(pending, 1, 11, 101),
		      "pending plan: keeps only manifests absent from depotcache");
	}

	// 8) Volume is not a policy limit.  A title may expose thousands of
	// manifests; if they are already staged, all are checked and none creates
	// background work.
	{
		std::vector<PICS::StageTarget> plan;
		for (uint32_t i = 0; i < 2000; ++i)
		{
			plan.push_back({ 311210, 400000 + i, 9000000 + i });
		}
		std::size_t checks = 0;
		const auto pending = PICS::buildPendingStagePlan(
		    plan,
		    [&](const PICS::StageTarget&)
		    {
		        ++checks;
		        return true;
		    });
		CHECK(checks == plan.size(),
		      "pending plan: checks every manifest without a count cap");
		CHECK(pending.empty(),
		      "pending plan: thousands of ready manifests create zero jobs");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
