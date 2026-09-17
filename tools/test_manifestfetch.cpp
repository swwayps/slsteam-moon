// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone test for the PURE manifest-fetch retry policy.
//
// Why this exists
// ---------------
// A manifest request-code resolves once and is cached by gid with no TTL,
// but Steam's CDN expires the code after ~5 min.  On a long install (a big,
// many-DLC title), Steam purges the unused DLC depots' manifests after the
// base commit; the background pre-warm worker then re-fetches them with the
// CDN auth is now independent from the manifest request code. A 401 can mean
// that the Steam-authenticated CDN token is missing, so HTTP status alone can
// no longer prove that the request code expired.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_manifestfetch.cpp -o /tmp/test_manifestfetch && /tmp/test_manifestfetch

#include "../src/utils/ManifestFetch.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using ManifestFetch::CdnOutcome;
using ManifestFetch::requiresSteamCdnAuth;
using ManifestFetch::RequestCodeCircuit;
using ManifestFetch::ProviderOutcome;
using ManifestFetch::isDefinitiveNotFound;
using ManifestFetch::looksLikeArchivedManifest;
using ManifestFetch::defaultProviderChain;
using ManifestFetch::expandProviderTemplate;
using ManifestFetch::appStateIsDownloading;
using ManifestFetch::cachedRequestCodeIsFresh;

int main()
{
	const auto& providers = defaultProviderChain();
	CHECK(!providers.empty(), "default request-code provider chain is not empty");
	CHECK(providers.front().find("{appid}/{depotid}/{gid}") != std::string::npos,
	      "first provider binds request codes to app, depot, and gid");
	CHECK(std::all_of(providers.begin(), providers.end(), [](const auto& provider)
	      {
		      return provider.find("{depotid}") != std::string::npos;
	      }),
	      "every automatic provider route is depot-aware");
	CHECK(expandProviderTemplate(providers.front(), 33, 11, 22).find("11/22/33") !=
	      std::string::npos,
	      "depot-aware provider template expands every identifier");

	CHECK(appStateIsDownloading(0x100), "update-running state is active");
	CHECK(appStateIsDownloading(0x400), "update-started state is active");
	CHECK(appStateIsDownloading(0x500), "combined update state is active");
	CHECK(!appStateIsDownloading(0x2), "update-required state alone is inactive");
	CHECK(!appStateIsDownloading(0x4), "fully-installed state alone is inactive");
	CHECK(cachedRequestCodeIsFresh(60000, 1),
	      "a request code remains available for the immediate paired consumer");
	CHECK(!cachedRequestCodeIsFresh(60001, 0),
	      "a request code expires from transient memory after one minute");
	CHECK(!cachedRequestCodeIsFresh(1000, 1001),
	      "a request code timestamp from the future is rejected");

	std::atomic<bool> bypass{false};
	ManifestFetch::detail::promoteArchiveMissBypass(bypass, false);
	CHECK(!bypass.load(), "background join does not promote archive retry");
	ManifestFetch::detail::promoteArchiveMissBypass(bypass, true);
	CHECK(bypass.load(), "active join promotes an existing blob job");
	ManifestFetch::detail::promoteArchiveMissBypass(bypass, false);
	CHECK(bypass.load(), "archive retry promotion is monotonic");
	std::atomic<uint32_t> appId{0};
	ManifestFetch::detail::promoteAppId(appId, 0);
	CHECK(appId.load() == 0, "missing app id does not replace the job context");
	ManifestFetch::detail::promoteAppId(appId, 11);
	CHECK(appId.load() == 11, "blob job retains a real app id for depot-aware lookup");
	ManifestFetch::detail::promoteAppId(appId, 22);
	CHECK(appId.load() == 11, "first real app id remains stable across joiners");

	std::string manifest("\xd0\x17\xf6\x71", 4);
	manifest.append(8, '\0');
	manifest.append("\xab\x15\xc4\x32", 4);
	CHECK(looksLikeArchivedManifest(manifest),
	      "archive accepts Steam payload and EOF markers");
	CHECK(!looksLikeArchivedManifest(manifest.substr(0, 15)),
	      "archive rejects a truncated body");
	manifest[0] = 0;
	CHECK(!looksLikeArchivedManifest(manifest),
	      "archive rejects a bad payload marker");
	manifest[0] = static_cast<char>(0xd0);
	manifest.back() = 0;
	CHECK(!looksLikeArchivedManifest(manifest),
	      "archive rejects a bad EOF marker");

	// Request-code provider circuit: 429 opens immediately, the cooldown does
	// not generate a synthetic gid=0 probe, and recovery is proven only by a
	// real successful request admitted after the deadline.
	{
		RequestCodeCircuit circuit(/*failureThreshold=*/2, /*cooldownMs=*/30000);
		const auto first = circuit.beginAttempt(1000);
		CHECK(first, "closed circuit admits the first real gid");
		circuit.finishAttempt(first, 1000, /*success=*/false,
		                     /*rateLimited=*/true, /*transportFailure=*/false);
		CHECK(circuit.open(), "HTTP 429 opens the request-code circuit immediately");
		CHECK(!circuit.beginAttempt(30999), "cooldown rejects requests without a probe");
		const auto halfOpen = circuit.beginAttempt(31000);
		CHECK(halfOpen, "cooldown admits one real half-open request");
		CHECK(!circuit.beginAttempt(31000), "only one half-open request runs at a time");
		circuit.finishAttempt(halfOpen, 31000, /*success=*/true,
		                     /*rateLimited=*/false, /*transportFailure=*/false);
		CHECK(!circuit.open() && circuit.beginAttempt(31001),
		      "a numeric real-gid success closes the circuit");
	}

	{
		RequestCodeCircuit circuit(2, 30000);
		const auto initial = circuit.beginAttempt(1000);
		CHECK(initial, "cancel test admits initial request");
		circuit.finishAttempt(initial, 1000, false, true, false);
		const auto oldHalfOpen = circuit.beginAttempt(31000);
		CHECK(oldHalfOpen, "cancel test admits half-open request");
		circuit.cancelAttempt(oldHalfOpen);
		const auto newHalfOpen = circuit.beginAttempt(31000);
		CHECK(newHalfOpen, "replacement half-open request is admitted");
		circuit.cancelAttempt(oldHalfOpen);
		CHECK(!circuit.beginAttempt(31000),
		      "an old token cannot release a newer half-open request");
		circuit.cancelAttempt(newHalfOpen);
		CHECK(circuit.beginAttempt(31000),
		      "session cancellation releases the half-open request immediately");
	}

	{
		RequestCodeCircuit circuit(2, 30000);
		const auto first = circuit.beginAttempt(0);
		CHECK(first, "first transport attempt admitted");
		circuit.finishAttempt(first, 0, false, false, true);
		CHECK(!circuit.open(), "one transport failure does not open the circuit");
		const auto second = circuit.beginAttempt(1);
		CHECK(second, "second transport attempt admitted");
		circuit.finishAttempt(second, 1, false, false, true);
		CHECK(circuit.open(), "two consecutive transport failures open the circuit");
	}

	{
		RequestCodeCircuit circuit(2, 30000);
		const auto attempt = circuit.beginAttempt(0);
		CHECK(attempt, "404-only attempt admitted");
		circuit.finishAttempt(attempt, 0, false, false, false);
		CHECK(!circuit.open(), "HTTP 404 alone is not a provider outage");
	}

	CHECK(isDefinitiveNotFound({{false, 404}}),
	      "a provider chain made entirely of 404 is definitive");
	CHECK(!isDefinitiveNotFound({{false, 429}, {false, 404}}),
	      "a rate limit followed by 404 is not a definitive gid miss");
	CHECK(!isDefinitiveNotFound({{true, 0}, {false, 404}}),
	      "a transport error followed by 404 is not a definitive gid miss");
	CHECK(!isDefinitiveNotFound({}),
	      "an empty provider chain cannot prove a gid is missing");

	CHECK(requiresSteamCdnAuth({false, 401}),
	      "HTTP 401 hands the manifest download back to Steam auth");
	CHECK(!requiresSteamCdnAuth({true, 0}),
	      "a transport error is not a Steam CDN auth challenge");
	CHECK(!requiresSteamCdnAuth({false, 503}),
	      "HTTP 503 is not a Steam CDN auth challenge");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
