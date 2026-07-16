// Standalone test for the pure manifest selection policy.
//
// Build:
//   g++ -std=c++20 -I include tools/test_manifestselection.cpp \
//       -o /tmp/test_manifestselection && /tmp/test_manifestselection

#include "../src/feats/manifestselection.hpp"

#include <cstdio>

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
