// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure freshness logic for the on-disk CM server-list cache.
//
// Why this exists
// ---------------
// CmClient::fetchProductInfoDetailed needs the websocket CM server list from
// ISteamDirectory/GetCMListForConnect, an HTTP round-trip of about 400 ms.
//
// The CM list is stable bootstrap infrastructure (a flat list of
// host:port endpoints), completely decoupled from per-app product-info.
// So it can be cached to disk for a long window and reused, skipping the
// HTTP GET on subsequent cold boots.  If every cached endpoint later
// fails to connect, CmClient re-fetches a fresh list, so a stale cache is
// self-healing rather than fatal.
//
// This header holds only the PURE decision (given presence + mtime + now
// + ttl, may we reuse the cached list?), so it is unit-testable without
// any I/O; the read/write/parse lives in cmclient.cpp.

#pragma once

namespace CmClient
{
namespace cache
{

// Decide whether an on-disk CM-list cache may be reused.
//   listExists : a non-empty cmlist cache file is present
//   mtimeSecs  : its last-modified time (epoch seconds)
//   nowSecs    : current time (epoch seconds)
//   ttlSecs    : freshness window; <= 0 disables the cache (always refetch)
// Mirrors provision_cache::isBufferReusable's semantics so the two caches
// behave identically: fresh within TTL → reuse; expired/missing/future
// mtime → refetch.
inline bool isCmListReusable(bool listExists, long long mtimeSecs,
                             long long nowSecs, long long ttlSecs)
{
	if (!listExists) return false;
	if (ttlSecs <= 0) return false;
	// A future mtime (clock skew / tampering) is not trusted.
	if (mtimeSecs > nowSecs) return false;
	const long long age = nowSecs - mtimeSecs;
	return age < ttlSecs;
}

} // namespace cache
} // namespace CmClient
