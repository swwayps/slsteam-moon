// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone test for the PURE manifest-fetch policies.
//
// Why this exists
// ---------------
// Manifests are staged from the luastools archive. The offline signal
// (areProvidersOffline) is derived from that archive's reachability: a pure
// classifier maps each archive HTTP result to the availability circuit's
// inputs, and the circuit opens after repeated transport failures and only
// closes once a reachable response is seen again. These policies are pure, so
// they are exercised here without touching the network.
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

using ManifestFetch::ProviderCircuit;
using ManifestFetch::looksLikeArchivedManifest;
using ManifestFetch::appStateIsDownloading;
using ManifestFetch::classifyArchiveOutcome;

int main()
{
	CHECK(appStateIsDownloading(0x100), "update-running state is active");
	CHECK(appStateIsDownloading(0x400), "update-started state is active");
	CHECK(appStateIsDownloading(0x500), "combined update state is active");
	CHECK(!appStateIsDownloading(0x2), "update-required state alone is inactive");
	CHECK(!appStateIsDownloading(0x4), "fully-installed state alone is inactive");

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

	// Provider availability circuit: 429 opens immediately, the cooldown admits
	// no probe until it elapses, and recovery is proven only by a reachable
	// response admitted after the deadline.
	{
		ProviderCircuit circuit(/*failureThreshold=*/2, /*cooldownMs=*/30000);
		const auto first = circuit.beginAttempt(1000);
		CHECK(first, "closed circuit admits the first probe");
		circuit.finishAttempt(first, 1000, /*success=*/false,
		                     /*rateLimited=*/true, /*transportFailure=*/false);
		CHECK(circuit.open(), "HTTP 429 opens the availability circuit immediately");
		CHECK(!circuit.beginAttempt(30999), "cooldown rejects requests without a probe");
		const auto halfOpen = circuit.beginAttempt(31000);
		CHECK(halfOpen, "cooldown admits one half-open probe");
		CHECK(!circuit.beginAttempt(31000), "only one half-open probe runs at a time");
		circuit.finishAttempt(halfOpen, 31000, /*success=*/true,
		                     /*rateLimited=*/false, /*transportFailure=*/false);
		CHECK(!circuit.open() && circuit.beginAttempt(31001),
		      "a reachable response closes the circuit");
	}

	{
		ProviderCircuit circuit(2, 30000);
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
		ProviderCircuit circuit(2, 30000);
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
		ProviderCircuit circuit(2, 30000);
		const auto attempt = circuit.beginAttempt(0);
		CHECK(attempt, "reachable attempt admitted");
		circuit.finishAttempt(attempt, 0, false, false, false);
		CHECK(!circuit.open(), "a single reachable response is not a provider outage");
	}

	// Archive availability classification: drives the offline circuit from the
	// luastools archive's reachability. The host is "reachable" whenever it
	// answers at all (200 staged, or 404 = manifest not uploaded yet); only a
	// transport error or a 5xx means we cannot reach it, and 429 is a rate limit.
	{
		const auto staged = classifyArchiveOutcome(false, 200);
		CHECK(staged.success && !staged.rateLimited && !staged.transportFailure,
		      "a 200 archive response counts the archive as reachable");
		const auto missing = classifyArchiveOutcome(false, 404);
		CHECK(missing.success && !missing.transportFailure && !missing.rateLimited,
		      "a 404 (manifest not archived yet) still counts the archive as reachable");
		const auto netErr = classifyArchiveOutcome(true, 0);
		CHECK(netErr.transportFailure && !netErr.success && !netErr.rateLimited,
		      "a transport error marks the archive unreachable");
		const auto serverErr = classifyArchiveOutcome(false, 503);
		CHECK(serverErr.transportFailure && !serverErr.success,
		      "a 5xx archive error counts the archive as unreachable");
		const auto limited = classifyArchiveOutcome(false, 429);
		CHECK(limited.rateLimited && !limited.success && !limited.transportFailure,
		      "a 429 archive response is a rate limit");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
