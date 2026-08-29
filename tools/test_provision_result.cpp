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
	using AppInfoProvision::ProvisionOutcome;
	using AppInfoProvision::ProvisionNotice;
	using AppInfoProvision::classifyContentResult;
	using AppInfoProvision::isProvisioned;
	using AppInfoProvision::isTerminalOutcome;
	using AppInfoProvision::noticeForOutcome;
	using AppInfoProvision::runtimePublicationAllowed;
	using AppInfoProvision::shouldTryProviderFallback;

	CHECK(classifyContentResult(false, true) == SourceResult::Success,
	      "usable content succeeds regardless of original source shape");
	CHECK(classifyContentResult(false, false) == SourceResult::IncompleteContent,
	      "missing concrete depot data remains fallback-eligible");
	CHECK(classifyContentResult(true, false) == SourceResult::NoUsableContent,
	      "concrete depots removed during pruning are terminal");
	CHECK(classifyContentResult(false, false, true) == SourceResult::VirtualDlc,
	      "virtual DLC without content is not treated as an incomplete game");
	CHECK(classifyContentResult(true, false, true) == SourceResult::NoUsableContent,
	      "DLC with concrete content removed remains terminal");

	CHECK(!shouldTryProviderFallback(SourceResult::Success),
	      "success does not fall back");
	CHECK(shouldTryProviderFallback(SourceResult::InvalidResponse),
	      "missing or malformed CM response falls back");
	CHECK(shouldTryProviderFallback(SourceResult::IncompleteContent),
	      "valid CM response missing depot data can fall back");
	CHECK(!shouldTryProviderFallback(SourceResult::NoUsableContent),
	      "concrete depots removed as unusable are terminal");
	CHECK(!shouldTryProviderFallback(SourceResult::VirtualDlc),
	      "virtual DLC is terminal without a provider retry");
	CHECK(!shouldTryProviderFallback(SourceResult::LocalFailure),
	      "local persistence failure is terminal");

	CHECK(isProvisioned(ProvisionOutcome::Updated),
	      "freshly updated appinfo counts as provisioned");
	CHECK(isProvisioned(ProvisionOutcome::FreshCache),
	      "fresh same-boot cache counts as provisioned");
	CHECK(isProvisioned(ProvisionOutcome::FallbackCache),
	      "validated offline fallback counts as provisioned");
	CHECK(noticeForOutcome(ProvisionOutcome::FallbackCache) == ProvisionNotice::None,
	      "validated fallback cache is silent");
	CHECK(noticeForOutcome(ProvisionOutcome::NetworkUnavailable) ==
	          ProvisionNotice::MetadataUnavailable,
	      "network failure without cache gets a connectivity notice");
	CHECK(noticeForOutcome(ProvisionOutcome::IncompleteContent) ==
	          ProvisionNotice::ReviewGameData,
	      "unusable depot data asks the user to review the game data");
	CHECK(noticeForOutcome(ProvisionOutcome::NotApplicable) == ProvisionNotice::None,
	      "virtual DLC without content is silent");
	CHECK(!isProvisioned(ProvisionOutcome::NotApplicable),
	      "virtual DLC is not counted as a provisioned game");
	CHECK(isTerminalOutcome(ProvisionOutcome::NotApplicable),
	      "virtual DLC is terminal and silent");
	CHECK(isTerminalOutcome(ProvisionOutcome::NoUsableContent),
	      "concrete unusable content is terminal but actionable");
	CHECK(noticeForOutcome(ProvisionOutcome::NoUsableContent) ==
	          ProvisionNotice::ReviewGameData,
	      "unusable content retains the review-data notice");
	CHECK(!isProvisioned(ProvisionOutcome::NoUsableContent),
	      "unusable content is resolved without becoming installable");
	CHECK(noticeForOutcome(ProvisionOutcome::LocalFailure) ==
	          ProvisionNotice::LocalStorage,
	      "cache write failure gets a local-storage notice");

	CHECK(runtimePublicationAllowed(true, ProvisionOutcome::Updated),
	      "a requested live publication accepts a newly published cache pair");
	CHECK(!runtimePublicationAllowed(false, ProvisionOutcome::Updated),
	      "an ordinary update remains disk-only");
	CHECK(runtimePublicationAllowed(true, ProvisionOutcome::FreshCache),
	      "a validated warm cache can complete a live hot-add");
	CHECK(runtimePublicationAllowed(true, ProvisionOutcome::FallbackCache),
	      "a validated offline fallback can complete a live hot-add");
	CHECK(!runtimePublicationAllowed(true, ProvisionOutcome::LocalFailure),
	      "a failed publication cannot trigger a live reload");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
