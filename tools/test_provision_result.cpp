// Standalone test for native-CM fallback decisions.
//
// Build (from repo root):
//   g++ -std=c++20 tools/test_provision_result.cpp -o /tmp/test_provision_result && /tmp/test_provision_result

#include "../src/feats/provision_result.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using AppInfoProvision::SourceResult;
	using AppInfoProvision::classifyContentResult;
	using AppInfoProvision::shouldTryProviderFallback;

	CHECK(classifyContentResult(false, true) == SourceResult::Success,
	      "usable content succeeds regardless of original source shape");
	CHECK(classifyContentResult(false, false) == SourceResult::IncompleteContent,
	      "missing concrete depot data remains fallback-eligible");
	CHECK(classifyContentResult(true, false) == SourceResult::NoUsableContent,
	      "concrete depots removed during pruning are terminal");

	CHECK(!shouldTryProviderFallback(SourceResult::Success),
	      "success does not fall back");
	CHECK(shouldTryProviderFallback(SourceResult::InvalidResponse),
	      "missing or malformed CM response falls back");
	CHECK(shouldTryProviderFallback(SourceResult::IncompleteContent),
	      "valid CM response missing depot data can fall back");
	CHECK(!shouldTryProviderFallback(SourceResult::NoUsableContent),
	      "concrete depots removed as unusable are terminal");
	CHECK(!shouldTryProviderFallback(SourceResult::LocalFailure),
	      "local persistence failure is terminal");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
