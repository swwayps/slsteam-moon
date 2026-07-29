// Standalone test for the pure provisioning-cache freshness logic
// (src/feats/provision_cache.hpp).
//
// Why this exists
// ---------------
// AppInfoProvision::provisionApp issues one synchronous HTTP GET to
// api.steamcmd.net per AdditionalApp, and Steam re-execs setup() several
// times during a single cold boot (observed 4x on the Zorin VM), so the
// startup cost is O(n_apps * n_setup_passes) network round-trips — it
// grows with every game the user adds and stalls Steam's launch.
//
// The fix: skip the fetch when a freshly-written picsbuffer_<appid>.bin
// is already on disk.  The freshness window (TTL) is short so the re-exec
// storm of ONE boot reuses the buffer (near-instant), while a genuine
// relaunch minutes/hours later (> TTL) still re-fetches the live gids
// (the install-first-attempt fix depends on the staged gid matching what
// Steam requests, so we must NOT serve a stale buffer across sessions).
//
// This pins down the PURE decision: given the buffer's presence + mtime,
// the current time and the TTL, do we reuse (skip the network)?  The
// actual stat()/HTTP/persist lives in appinfo_provision.cpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_provision_cache.cpp -o /tmp/test_provision_cache && /tmp/test_provision_cache

#include "../src/feats/provision_cache.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using AppInfoProvision::cache::CacheUse;
	using AppInfoProvision::cache::CacheRecordFacts;
	using AppInfoProvision::cache::chooseCacheUse;
	using AppInfoProvision::cache::isBufferReusable;
	using AppInfoProvision::cache::isCacheRecordValid;

	const long long ttl = 300; // 5 minutes
	const long long now = 1'000'000;

	// A missing/empty buffer can never be reused — we have nothing to serve.
	CHECK(!isBufferReusable(/*bufExists=*/false, now - 1, now, ttl),
	      "missing buffer is not reusable");

	// A buffer written just now is reusable (collapses the boot re-exec storm).
	CHECK(isBufferReusable(true, now, now, ttl),
	      "buffer written now is reusable");

	// A buffer written within the TTL window is reusable.
	CHECK(isBufferReusable(true, now - (ttl - 1), now, ttl),
	      "buffer aged just under TTL is reusable");

	// At exactly the TTL boundary the buffer has expired (age == ttl is NOT < ttl).
	CHECK(!isBufferReusable(true, now - ttl, now, ttl),
	      "buffer aged exactly TTL is not reusable");

	// Past the TTL the buffer is stale — re-fetch live gids for this session.
	CHECK(!isBufferReusable(true, now - (ttl + 1), now, ttl),
	      "buffer aged past TTL is not reusable");

	// A non-positive TTL disables the cache entirely (always re-fetch).
	CHECK(!isBufferReusable(true, now, now, 0),
	      "ttl=0 disables reuse");
	CHECK(!isBufferReusable(true, now, now, -5),
	      "negative ttl disables reuse");

	// A buffer whose mtime is in the future (clock skew / tampering) must not
	// be trusted — fall back to a fetch rather than serve an unverifiable file.
	CHECK(!isBufferReusable(true, now + 10, now, ttl),
	      "future mtime is not trusted");

	// A cryptographically/structurally valid stale buffer is a contingency,
	// not the normal online source. This catches either extreme: rejecting
	// every cross-session cache while offline, or silently hiding updates by
	// preferring stale data while refresh is available.
	CHECK(chooseCacheUse(true, true, false) == CacheUse::Fresh,
	      "fresh valid cache skips duplicate work in the same boot");
	CHECK(chooseCacheUse(true, false, false) == CacheUse::None,
	      "stale cache does not hide online updates");
	CHECK(chooseCacheUse(true, false, true) == CacheUse::Fallback,
	      "stale valid cache is accepted after refresh becomes unavailable");
	CHECK(chooseCacheUse(false, false, true) == CacheUse::None,
	      "invalid cache is never accepted as an offline fallback");

	const CacheRecordFacts validRecord{
	    .requestedAppId = 420530,
	    .metadataAppId = 420530,
	    .declaredSize = 8192,
	    .actualSize = 8192,
	    .shaSize = 20,
	    .shaMatches = true,
	    .parsed = true,
	    .hasUsableContent = true,
	};
	CHECK(isCacheRecordValid(validRecord),
	      "matching metadata, digest and content form a valid cache record");
	{
		auto facts = validRecord;
		facts.metadataAppId = 588650;
		CHECK(!isCacheRecordValid(facts),
		      "cache metadata for another AppID is rejected");
	}
	{
		auto facts = validRecord;
		facts.actualSize = 8191;
		CHECK(!isCacheRecordValid(facts),
		      "truncated cache buffer is rejected");
	}
	{
		auto facts = validRecord;
		facts.shaMatches = false;
		CHECK(!isCacheRecordValid(facts),
		      "cache buffer with a mismatched SHA-1 is rejected");
	}
	{
		auto facts = validRecord;
		facts.hasUsableContent = false;
		CHECK(!isCacheRecordValid(facts),
		      "cache buffer without a concrete manifest is rejected");
	}

	if (g_failures == 0) std::printf("\nall provision-cache checks passed\n");
	else                 std::printf("\n%d provision-cache check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
