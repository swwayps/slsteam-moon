// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone test for the PURE manifest-fetch retry policy
// (src/utils/ManifestFetch.hpp :: isExpiredCodeSignature).
//
// Why this exists
// ---------------
// A manifest request-code resolves once and is cached by gid with no TTL,
// but Steam's CDN expires the code after ~5 min.  On a long install (a big,
// many-DLC title), Steam purges the unused DLC depots' manifests after the
// base commit; the background pre-warm worker then re-fetches them with the
// now-stale code and the CDN answers HTTP 401 on EVERY host.  A unanimous
// 401 is the recoverable "expired code" signature — re-resolve a fresh code
// and retry — whereas a network error or any non-401 status is a real /
// transient failure a refresh would not fix.  This pins that decision down
// without needing libcurl or the network.
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
using ManifestFetch::isExpiredCodeSignature;
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

	// No attempts at all is not the expired-code signature (nothing tried).
	CHECK(!isExpiredCodeSignature({}),
	      "empty outcome set is not an expired-code signature");

	// Every host returned 401 -> expired code, worth a refresh+retry.
	{
		const std::vector<CdnOutcome> all401 = {
			{false, 401}, {false, 401}, {false, 401},
			{false, 401}, {false, 401}, {false, 401},
		};
		CHECK(isExpiredCodeSignature(all401),
		      "all hosts 401 -> expired-code signature");
	}

	// A single host (e.g. only one tried before break) at 401 still counts.
	CHECK(isExpiredCodeSignature({ {false, 401} }),
	      "single 401 -> expired-code signature");

	// A 503 (overloaded edge) mixed in means it is NOT purely a code issue;
	// a refresh would not help, so don't treat it as expired-code.
	{
		const std::vector<CdnOutcome> mixed = {
			{false, 401}, {false, 503}, {false, 401},
		};
		CHECK(!isExpiredCodeSignature(mixed),
		      "401 mixed with 503 is not an expired-code signature");
	}

	// A network error (curl failed, status meaningless) is not a code issue.
	{
		const std::vector<CdnOutcome> netErr = {
			{false, 401}, {true, 0}, {false, 401},
		};
		CHECK(!isExpiredCodeSignature(netErr),
		      "401 mixed with a network error is not an expired-code signature");
	}

	// All hosts 503 (none 401) is a transient CDN problem, not expired code.
	{
		const std::vector<CdnOutcome> all503 = {
			{false, 503}, {false, 503}, {false, 503},
		};
		CHECK(!isExpiredCodeSignature(all503),
		      "all hosts 503 is not an expired-code signature");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
