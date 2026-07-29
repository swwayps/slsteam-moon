// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure freshness logic for the AdditionalApps provisioning cache.
//
// AppInfoProvision::provisionApp does one synchronous HTTP GET per added
// app, and Steam re-execs setup() several times during a single cold boot,
// so the naive cost is O(n_apps * n_passes) network round-trips that grow
// with every game the user adds.  We short-circuit the fetch when a
// freshly-written picsbuffer_<appid>.bin is already on disk.
//
// The window (TTL) is intentionally short: it must cover the re-exec storm
// of ONE boot (so those passes reuse the buffer and Steam launches fast)
// without surviving into a later genuine relaunch — across sessions we
// re-fetch the live public gid, because the install-first-attempt path
// stages whatever gid the buffer carries and Steam refreshes appinfo to
// the live gid at install time, so a stale cross-session buffer would
// reintroduce the gid mismatch this project already fixed.
//
// This header is PURE (no I/O) so the decision is unit-testable; the
// stat()/fetch/persist wiring lives in appinfo_provision.cpp.

#pragma once

namespace AppInfoProvision
{
namespace cache
{

enum class CacheUse
{
	None,
	Fresh,
	Fallback,
};

struct CacheRecordFacts
{
	unsigned int requestedAppId = 0;
	unsigned int metadataAppId = 0;
	unsigned long long declaredSize = 0;
	unsigned long long actualSize = 0;
	unsigned long long shaSize = 0;
	bool shaMatches = false;
	bool parsed = false;
	bool hasUsableContent = false;
};

inline bool isCacheRecordValid(const CacheRecordFacts& facts)
{
	return facts.requestedAppId != 0
	    && facts.metadataAppId == facts.requestedAppId
	    && facts.declaredSize > 0
	    && facts.declaredSize == facts.actualSize
	    && facts.shaSize == 20
	    && facts.shaMatches
	    && facts.parsed
	    && facts.hasUsableContent;
}

// A stale buffer may preserve the last known-good appinfo only after the live
// refresh path is unavailable. It must never win while online, where doing so
// would conceal new change numbers and manifest gids.
inline CacheUse chooseCacheUse(bool cacheValid, bool cacheFresh,
                               bool refreshUnavailable)
{
	if (!cacheValid) return CacheUse::None;
	if (cacheFresh) return CacheUse::Fresh;
	return refreshUnavailable ? CacheUse::Fallback : CacheUse::None;
}

// Decide whether an existing provisioned buffer can be reused (i.e. the
// network fetch can be skipped) given:
//   bufExists  — whether picsbuffer_<appid>.bin is present and non-empty
//   mtimeSecs  — that file's last-modified time, in epoch seconds
//   nowSecs    — current time, in epoch seconds
//   ttlSecs    — freshness window; <= 0 disables the cache entirely
//
// Reuse only when the buffer exists and its age is strictly within the
// TTL.  A future mtime (clock skew / tampering) is not trusted.
inline bool isBufferReusable(bool bufExists, long long mtimeSecs,
                             long long nowSecs, long long ttlSecs)
{
	if (!bufExists)    return false;
	if (ttlSecs <= 0)  return false;

	const long long age = nowSecs - mtimeSecs;
	if (age < 0)       return false;   // mtime in the future — don't trust it
	return age < ttlSecs;
}

} // namespace cache
} // namespace AppInfoProvision
