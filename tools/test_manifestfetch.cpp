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

int main()
{
	// Request-code provider circuit: 429 opens immediately, the cooldown does
	// not generate a synthetic gid=0 probe, and recovery is proven only by a
	// real successful request admitted after the deadline.
	{
		RequestCodeCircuit circuit(/*failureThreshold=*/2, /*cooldownMs=*/30000);
		CHECK(circuit.beginAttempt(1000), "closed circuit admits the first real gid");
		circuit.finishAttempt(1000, /*success=*/false,
		                     /*rateLimited=*/true, /*transportFailure=*/false);
		CHECK(circuit.open(), "HTTP 429 opens the request-code circuit immediately");
		CHECK(!circuit.beginAttempt(30999), "cooldown rejects requests without a probe");
		CHECK(circuit.beginAttempt(31000), "cooldown admits one real half-open request");
		CHECK(!circuit.beginAttempt(31000), "only one half-open request runs at a time");
		circuit.finishAttempt(31000, /*success=*/true,
		                     /*rateLimited=*/false, /*transportFailure=*/false);
		CHECK(!circuit.open() && circuit.beginAttempt(31001),
		      "a numeric real-gid success closes the circuit");
	}

	{
		RequestCodeCircuit circuit(2, 30000);
		CHECK(circuit.beginAttempt(0), "first transport attempt admitted");
		circuit.finishAttempt(0, false, false, true);
		CHECK(!circuit.open(), "one transport failure does not open the circuit");
		CHECK(circuit.beginAttempt(1), "second transport attempt admitted");
		circuit.finishAttempt(1, false, false, true);
		CHECK(circuit.open(), "two consecutive transport failures open the circuit");
	}

	{
		RequestCodeCircuit circuit(2, 30000);
		CHECK(circuit.beginAttempt(0), "404-only attempt admitted");
		circuit.finishAttempt(0, false, false, false);
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
