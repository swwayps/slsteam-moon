// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure freshness logic for the Updater's safe-mode-hash cache.
//
// Updater::init() used to do a SYNCHRONOUS HTTPS GET to GitHub
// (res/updates.yaml) on EVERY Steam boot, on the LD_AUDIT preinit path,
// blocking Steam's launch on network latency.  Its only product is the
// SafeModeHashes map, which is consulted by verifySafeModeHash() — and
// that result is only acted on when the user has SafeMode or
// WarnHashMissmatch enabled (both default off).  For the default user the
// fetch is pure boot-time cost with no behavioural effect.
//
// The fix: serve the on-disk cache synchronously at boot (no network),
// and refresh it in the BACKGROUND from a real Steam worker thread, but
// only when the cache is older than a TTL (so we don't hammer GitHub on
// every relaunch).  When SafeMode/WarnHashMissmatch are on, the caller
// still fetches synchronously so the hash data is authoritative right now.
//
// This header is PURE (no I/O) so the decision is unit-testable; the
// file-read / HTTP / thread wiring lives in update.cpp.

#pragma once

namespace Updater
{
namespace cache
{

// Decide whether the cached updates.yaml is still fresh enough to skip a
// background network refresh, given:
//   cacheExists — whether .updates.yaml is present and non-empty on disk
//   mtimeSecs   — that file's last-modified time, in epoch seconds
//   nowSecs     — current time, in epoch seconds
//   ttlSecs     — freshness window; <= 0 forces a refresh every time
//
// Fresh only when the cache exists and its age is strictly within the
// TTL.  A future mtime (clock skew / tampering) is treated as NOT fresh
// so we re-fetch rather than trust an unverifiable timestamp.
inline bool isCacheFresh(bool cacheExists, long long mtimeSecs,
                         long long nowSecs, long long ttlSecs)
{
	if (!cacheExists)  return false;
	if (ttlSecs <= 0)  return false;

	const long long age = nowSecs - mtimeSecs;
	if (age < 0)       return false;   // mtime in the future — don't trust it
	return age < ttlSecs;
}

// Decide whether Updater must fetch SYNCHRONOUSLY at boot (blocking) vs.
// deferring to a background refresh.  We must block only when the
// safe-mode hash data is actually going to gate behaviour this session:
//   - safeMode: abort the client on an unknown steamclient.so hash.
//   - warnHashMissmatch: warn on mismatch.
// In either case the map has to be authoritative before load() runs its
// verifySafeModeHash() check.  Otherwise (the default), the cache is good
// enough to start with and the refresh can happen off the critical path.
inline bool mustFetchSynchronously(bool safeMode, bool warnHashMissmatch)
{
	return safeMode || warnHashMissmatch;
}

// Decide whether load() has to SHA-256 the whole of steamclient.so.
//
// Measured on the CachyOS test VM: hashing the 49 MB steamclient.so costs
// 0.22 s (median of 13 boots) of BLOCKING work inside the client's
// dlopen(steamclient.so) — i.e. it delays Steam's own startup, on the
// client's main thread.  The digest is only ever consumed by the
// SafeMode-abort and WarnHashMissmatch-warn branches in main.cpp::load(),
// and both settings default to `no`, so the default install pays 0.22 s
// per boot for a value nobody reads.
//
// Keep the hash when either flag is on (the behaviour they gate needs it)
// and when ExtendedLogging is on (the `steamclient.so hash is …` log line
// is a diagnostic people ask for).  Otherwise skip it.
inline bool mustVerifyClientHash(bool safeMode, bool warnHashMissmatch,
                                 bool extendedLogging)
{
	return safeMode || warnHashMissmatch || extendedLogging;
}

} // namespace cache
} // namespace Updater
