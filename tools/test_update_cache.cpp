// Standalone test for the pure Updater cache-freshness / fetch-mode logic
// (src/update_cache.hpp).
//
// Why this exists
// ---------------
// Updater::init() used to do a synchronous HTTPS GET to GitHub on every
// boot, on the preinit critical path, blocking Steam's launch.  Its only
// product (SafeModeHashes) is consulted by verifySafeModeHash(), which
// only gates behaviour when SafeMode / WarnHashMissmatch are enabled
// (both default off).  So for the default user the fetch is pure boot
// cost with no effect.
//
// The fix serves the disk cache at boot and refreshes in the background
// only when the cache is older than a TTL, and only blocks synchronously
// when the hash data must be authoritative this session.  This pins down
// the two PURE decisions; the I/O lives in update.cpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_update_cache.cpp -o /tmp/test_update_cache && /tmp/test_update_cache

#include "../src/update_cache.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using Updater::cache::isCacheFresh;
	using Updater::cache::mustFetchSynchronously;

	const long long ttl = 24 * 3600; // 1 day
	const long long now = 1'000'000'000;

	// --- isCacheFresh -----------------------------------------------------
	CHECK(!isCacheFresh(/*cacheExists=*/false, now - 1, now, ttl),
	      "missing cache is never fresh");
	CHECK(isCacheFresh(true, now, now, ttl),
	      "cache written now is fresh");
	CHECK(isCacheFresh(true, now - (ttl - 1), now, ttl),
	      "cache aged just under TTL is fresh");
	CHECK(!isCacheFresh(true, now - ttl, now, ttl),
	      "cache aged exactly TTL is not fresh");
	CHECK(!isCacheFresh(true, now - (ttl + 1), now, ttl),
	      "cache aged past TTL is not fresh");
	CHECK(!isCacheFresh(true, now, now, 0),
	      "ttl=0 forces refresh");
	CHECK(!isCacheFresh(true, now, now, -5),
	      "negative ttl forces refresh");
	CHECK(!isCacheFresh(true, now + 10, now, ttl),
	      "future mtime is not trusted");

	// --- mustFetchSynchronously ------------------------------------------
	CHECK(!mustFetchSynchronously(/*safeMode=*/false, /*warnHashMissmatch=*/false),
	      "default user (both off) defers the fetch");
	CHECK(mustFetchSynchronously(true, false),
	      "SafeMode forces a synchronous fetch");
	CHECK(mustFetchSynchronously(false, true),
	      "WarnHashMissmatch forces a synchronous fetch");
	CHECK(mustFetchSynchronously(true, true),
	      "both on forces a synchronous fetch");

	// --- mustVerifyClientHash --------------------------------------------
	// 0.22 s of blocking preinit (SHA-256 of the 49 MB steamclient.so) is
	// only worth paying when something actually consumes the digest.
	using Updater::cache::mustVerifyClientHash;
	CHECK(!mustVerifyClientHash(/*safeMode=*/false, /*warnHashMissmatch=*/false,
	                            /*extendedLogging=*/false),
	      "default config skips the steamclient.so hash");
	CHECK(mustVerifyClientHash(true, false, false),
	      "SafeMode still hashes (it aborts on an unknown hash)");
	CHECK(mustVerifyClientHash(false, true, false),
	      "WarnHashMissmatch still hashes (it warns on mismatch)");
	CHECK(mustVerifyClientHash(false, false, true),
	      "ExtendedLogging still hashes (diagnostic log line)");
	CHECK(mustVerifyClientHash(true, true, true),
	      "everything on still hashes");

	if (g_failures == 0) std::printf("\nall update-cache checks passed\n");
	else                 std::printf("\n%d update-cache check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
