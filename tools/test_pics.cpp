// Standalone test for the pure PICS response policy (src/feats/pics.hpp).
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_pics.cpp -o /tmp/test_pics && /tmp/test_pics

#include "../src/feats/pics.hpp"

#include <cstdio>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

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

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
