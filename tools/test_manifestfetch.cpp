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

int main()
{
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
