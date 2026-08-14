// Standalone test for the pure manifest selection policy.
//
// Build:
//   g++ -std=c++20 -I src tools/test_manifestselection.cpp -o test_manifestselection
//   ./test_manifestselection

#include "../src/feats/manifestselection.hpp"

#include <cstdio>
#include <optional>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using ManifestSelection::ChoiceSource;
using ManifestSelection::ExactState;

int main()
{
	// The exact public gid always wins when it is ready, even if older local
	// versions are available.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Ready, /*preferredLocal=*/800, /*legacyLocal=*/700);
		CHECK(d.final && d.gid == 900 && d.source == ChoiceSource::Exact,
		      "ready exact gid wins over every local fallback");
	}

	// While the exact fetch is still running, do not prematurely freeze a
	// fallback decision. The caller may wait within the shared plan budget.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Pending, /*preferredLocal=*/800, /*legacyLocal=*/700);
		CHECK(!d.final && d.gid == 900,
		      "pending exact gid remains unresolved");
	}

	// Once the exact gid is confirmed unavailable (404, provider outage, or
	// plan timeout), prefer the last server-observed local gid.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Unavailable,
		    /*preferredLocal=*/800, /*legacyLocal=*/850);
		CHECK(d.final && d.gid == 800 &&
		          d.source == ChoiceSource::PreferredLocal,
		      "unavailable exact gid falls back to last observed local gid");
	}

	// Gids are opaque identifiers, not sortable version counters. The
	// preferred marker wins even when a legacy file has a numerically larger
	// gid.
	{
		const auto d = ManifestSelection::choose(
		    100, ExactState::Unavailable,
		    /*preferredLocal=*/20, /*legacyLocal=*/999999);
		CHECK(d.gid == 20 && d.source == ChoiceSource::PreferredLocal,
		      "manifest recency never depends on numeric gid ordering");
	}

	// Existing users may have manifests without preferred metadata. Preserve
	// the legacy newest-archived fallback as the compatibility path.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Unavailable,
		    /*preferredLocal=*/0, /*legacyLocal=*/700);
		CHECK(d.gid == 700 && d.source == ChoiceSource::LegacyLocal,
		      "legacy archived manifest is used when no preferred marker exists");
	}

	// A corrupt preferred marker pointing at the failed exact gid must not
	// suppress a usable older local manifest.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Unavailable,
		    /*preferredLocal=*/900, /*legacyLocal=*/700);
		CHECK(d.gid == 700 && d.source == ChoiceSource::LegacyLocal,
		      "failed exact gid is excluded from fallback candidates");
	}

	// Worst case: no local manifest and no exact manifest. Keep the requested
	// gid so Steam follows its normal error/retry path; inventing a gid would
	// be unsafe.
	{
		const auto d = ManifestSelection::choose(
		    900, ExactState::Unavailable, 0, 0);
		CHECK(d.final && d.gid == 900 &&
		          d.source == ChoiceSource::ExactUnavailable,
		      "no local fallback leaves the requested gid unchanged");
	}

	// One budget is shared by the whole plan. It is clamped at zero instead
	// of restarting for every depot.
	CHECK(ManifestSelection::remainingBudgetMs(12000, 3000) == 9000,
	      "plan budget subtracts elapsed time");
	CHECK(ManifestSelection::remainingBudgetMs(12000, 12000) == 0,
	      "plan budget reaches zero at the deadline");
	CHECK(ManifestSelection::remainingBudgetMs(12000, 15000) == 0,
	      "plan budget never becomes negative");

	// A first-import pin has no cached size until its asynchronous manifest
	// fetch completes. The initial Validate must update both fields after the
	// bounded join, without requiring a second Validate or restart.
	{
		std::optional<uint64_t> size;
		int waits = 0;
		const auto pair = ManifestSelection::resolvePinnedPair(
		    100, 1000, 200, 9000,
		    [&]() { return size; },
		    [&](int budgetMs)
		    {
			++waits;
			CHECK(budgetMs == 9000,
			      "first-import wait receives only the remaining plan budget");
			size = 2222;
			return true;
		    });
		CHECK(waits == 1 && pair.pinned && pair.gid == 200 && pair.size == 2222,
		      "first-import fetch applies pinned gid and size in one plan");
	}

	// Failure and timeout are deliberately fail-open: Steam's complete public
	// pair survives, rather than a pinned gid being combined with public size.
	for (const int budget : {9000, 0})
	{
		int waits = 0;
		const auto pair = ManifestSelection::resolvePinnedPair(
		    100, 1000, 200, budget,
		    []() { return std::optional<uint64_t>{}; },
		    [&](int) { ++waits; return false; });
		CHECK(!pair.pinned && pair.gid == 100 && pair.size == 1000,
		      "unavailable pinned metadata preserves the public pair atomically");
		CHECK(waits == (budget > 0 ? 1 : 0),
		      "an exhausted shared budget never starts another wait");
	}
	{
		const auto pair = ManifestSelection::resolvePinnedPair(
		    100, 1000, 200, 9000,
		    []() { return std::optional<uint64_t>{}; },
		    [](int) { return true; });
		CHECK(!pair.pinned && pair.gid == 100 && pair.size == 1000,
		      "fetched manifest without readable size preserves the public pair");
	}

	// Cached metadata is the hot path: no provider wait is performed.
	{
		int waits = 0;
		const auto pair = ManifestSelection::resolvePinnedPair(
		    100, 1000, 200, 9000,
		    []() { return std::optional<uint64_t>{2222}; },
		    [&](int) { ++waits; return true; });
		CHECK(waits == 0 && pair.pinned && pair.gid == 200 && pair.size == 2222,
		      "cached pinned size avoids the blocking provider path");
	}

	// Depot count is validated against CUtlVector's actual allocation, not a
	// product-policy cap. Large valid plans remain eligible for staging.
	CHECK(ManifestSelection::validVectorBounds(5000, 5000, 0x20),
	      "valid depot vectors are not capped at 4096 entries");
	CHECK(!ManifestSelection::validVectorBounds(5001, 5000, 0x20),
	      "depot vector count cannot exceed its allocation");
	CHECK(!ManifestSelection::validVectorBounds(-1, 5000, 0x20),
	      "negative depot vector counts are rejected");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
