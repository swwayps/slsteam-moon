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
// The window (TTL) is intentionally short and applies only to legacy/full
// refresh passes inside one boot. Startup splice eligibility is structural,
// not time-based: a complete pair is validated and reused across sessions,
// while explicit PICS change numbers schedule targeted refreshes. Install-time
// manifest selection remains authoritative and can fall back to the locally
// installed/archive artifact when the current public artifact is unavailable.
//
// This header is PURE (no I/O) so the decision is unit-testable; the
// stat()/fetch/persist wiring lives in appinfo_provision.cpp.

#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

namespace AppInfoProvision
{

// Complete readiness classification used by the refresh scheduler. The cache
// probe currently produces the established Missing/Fresh/ValidStale states;
// the remaining states let later probe work distinguish contention and
// unverified data without treating either as a normal warm cache.
enum class CacheReadiness
{
	Missing,
	Invalid,
	Fresh,
	ValidStale,
	Busy,
	Unverified,
};

namespace cache
{

struct CacheMetadataView
{
	std::uint32_t appId = 0;
	std::uint32_t changeNumber = 0;
	std::uint64_t wireSize = 0;
	std::string_view shaBase64;
	bool hasNormalized = false;
	bool normalized = false;
	bool hasSynthetic = false;
	bool synthetic = false;
};

// Parse the small metadata format emitted by persistBuffer without invoking
// yaml-cpp. YAML::LoadFile reports a missing/malformed file by throwing; the
// portable optimized build cannot reliably unwind that throw from a watcher
// pthread, so a cache miss must be represented as an ordinary false result.
inline bool parseCacheMetadata(
	std::string_view text,
	CacheMetadataView& output) noexcept
{
	CacheMetadataView parsed;
	bool appIdSeen = false;
	bool wireSizeSeen = false;
	bool shaSeen = false;
	bool changeSeen = false;
	bool normalizedSeen = false;
	bool syntheticSeen = false;

	const auto parseUnsigned = [](
		std::string_view value,
		std::uint64_t& result) noexcept
	{
		if (value.empty()) return false;
		result = 0;
		const auto converted = std::from_chars(
			value.data(), value.data() + value.size(), result, 10);
		return converted.ec == std::errc{} &&
			converted.ptr == value.data() + value.size();
	};
	const auto parseBool = [](
		std::string_view value,
		bool& result) noexcept
	{
		if (value == "true") { result = true; return true; }
		if (value == "false") { result = false; return true; }
		return false;
	};

	std::size_t cursor = 0;
	while (cursor < text.size())
	{
		const std::size_t newline = text.find('\n', cursor);
		const std::size_t end = newline == std::string_view::npos
			? text.size() : newline;
		const std::string_view line = text.substr(cursor, end - cursor);
		cursor = newline == std::string_view::npos ? text.size() : newline + 1;
		if (line.empty() || line.find('\r') != std::string_view::npos)
			return false;

		const std::size_t separator = line.find(": ");
		if (separator == std::string_view::npos || separator == 0 ||
			separator + 2 >= line.size())
		{
			return false;
		}
		const std::string_view key = line.substr(0, separator);
		const std::string_view value = line.substr(separator + 2);
		std::uint64_t number = 0;

		if (key == "appid")
		{
			if (appIdSeen || !parseUnsigned(value, number) || number == 0 ||
				number > std::numeric_limits<std::uint32_t>::max())
				return false;
			appIdSeen = true;
			parsed.appId = static_cast<std::uint32_t>(number);
		}
		else if (key == "change_number")
		{
			if (changeSeen || !parseUnsigned(value, number) ||
				number > std::numeric_limits<std::uint32_t>::max())
				return false;
			changeSeen = true;
			parsed.changeNumber = static_cast<std::uint32_t>(number);
		}
		else if (key == "wire_size")
		{
			if (wireSizeSeen || !parseUnsigned(value, number) || number == 0)
				return false;
			wireSizeSeen = true;
			parsed.wireSize = number;
		}
		else if (key == "sha_b64")
		{
			// A SHA-1 digest is exactly 20 bytes, whose canonical Base64
			// representation is 27 alphabet characters plus one '='.  Reject
			// every other padding layout here so from_base64 cannot throw later.
			if (shaSeen || value.size() != 28 || value.back() != '=')
				return false;
			for (const unsigned char byte : value.substr(0, value.size() - 1))
			{
				const bool valid =
					(byte >= 'A' && byte <= 'Z') ||
					(byte >= 'a' && byte <= 'z') ||
					(byte >= '0' && byte <= '9') || byte == '+' || byte == '/';
				if (!valid) return false;
			}
			shaSeen = true;
			parsed.shaBase64 = value;
		}
		else if (key == "normalized")
		{
			if (normalizedSeen || !parseBool(value, parsed.normalized))
				return false;
			normalizedSeen = true;
			parsed.hasNormalized = true;
		}
		else if (key == "synthetic")
		{
			if (syntheticSeen || !parseBool(value, parsed.synthetic))
				return false;
			syntheticSeen = true;
			parsed.hasSynthetic = true;
		}
		else
		{
			return false;
		}
	}

	if (!appIdSeen || !wireSizeSeen || !shaSeen)
		return false;
	output = parsed;
	return true;
}

// Identity of the complete on-disk pair used as the memoization key for its
// expensive YAML/SHA-1/VDF validation. Both files participate: replacing only
// metadata must not inherit validation from the same buffer inode.
struct CacheValidationKey
{
	uint32_t appId = 0;
	long long mtimeSecs = 0;
	long long mtimeNsecs = 0;
	long long size = 0;
	std::uint64_t inode = 0;
	long long metadataMtimeSecs = 0;
	long long metadataMtimeNsecs = 0;
	long long metadataSize = 0;
	std::uint64_t metadataInode = 0;

	bool operator==(const CacheValidationKey& other) const noexcept
	{
		return appId == other.appId
		    && mtimeSecs == other.mtimeSecs
		    && mtimeNsecs == other.mtimeNsecs
		    && size == other.size
		    && inode == other.inode
		    && metadataMtimeSecs == other.metadataMtimeSecs
		    && metadataMtimeNsecs == other.metadataMtimeNsecs
		    && metadataSize == other.metadataSize
		    && metadataInode == other.metadataInode;
	}

	bool operator<(const CacheValidationKey& other) const noexcept
	{
		if (appId != other.appId) return appId < other.appId;
		if (mtimeSecs != other.mtimeSecs) return mtimeSecs < other.mtimeSecs;
		if (mtimeNsecs != other.mtimeNsecs)
			return mtimeNsecs < other.mtimeNsecs;
		if (size != other.size) return size < other.size;
		if (inode != other.inode) return inode < other.inode;
		if (metadataMtimeSecs != other.metadataMtimeSecs)
			return metadataMtimeSecs < other.metadataMtimeSecs;
		if (metadataMtimeNsecs != other.metadataMtimeNsecs)
			return metadataMtimeNsecs < other.metadataMtimeNsecs;
		if (metadataSize != other.metadataSize)
			return metadataSize < other.metadataSize;
		return metadataInode < other.metadataInode;
	}
};

enum class CacheUse
{
	None,
	Fresh,
	Fallback,
};

// A metadata record written before the provenance marker was introduced is
// ambiguous: it may be a provider-normalized pair or a raw PICS pair. Keep
// it authoritative against raw replacement until a provider refresh writes
// an explicit marker. Explicit raw records remain replaceable.
inline bool shouldPreserveCacheFromRawPics(bool hasNormalizedMarker,
                                           bool normalized)
{
	return !hasNormalizedMarker || normalized;
}

struct LocalAuthorityFacts
{
	bool managed = false;
	bool active = false;
	bool synthetic = false;
	bool cacheValid = false;
	bool hasNormalizedMarker = false;
	bool normalized = false;
};

inline bool locallyAuthoritative(const LocalAuthorityFacts& facts) noexcept
{
	if (!facts.active) return false;
	if (facts.synthetic) return true;
	return facts.managed && facts.cacheValid &&
		shouldPreserveCacheFromRawPics(
			facts.hasNormalizedMarker, facts.normalized);
}

// Permit a cache publication only when the caller still represents the
// managed app generation that started the work. A matching generation is
// required even when the app is managed again after a removal.
inline bool cachePublicationAllowed(bool managed,
                                    std::uint64_t expectedGeneration,
                                    std::uint64_t currentGeneration)
{
	return managed && expectedGeneration == currentGeneration;
}

inline bool protonPublicationAllowed(bool managed,
                                      std::uint64_t expectedGeneration,
                                      std::uint64_t currentGeneration)
{
	return cachePublicationAllowed(managed, expectedGeneration,
	                               currentGeneration);
}

// A terminal provisioning result — a DLC that carries only ownership metadata,
// or concrete depots none of which are usable here — can never publish a cache
// pair. Without remembering it, the app re-enters the cold set on every pass
// and pays a fresh CM round-trip forever, which also keeps the pass permanently
// "incomplete" and the retry backoff permanently armed.
//
// The memory is scoped to the publication generation so removing and re-adding
// the app, or any other event that bumps the generation, retries it once.
inline bool terminalResultStillApplies(bool recorded,
                                       std::uint64_t recordedGeneration,
                                       std::uint64_t currentGeneration)
{
	return recorded && recordedGeneration == currentGeneration;
}

// A cache pair and its provenance marker are one logical publication. A
// reader must reject either half of an interrupted transition: a synthetic
// pair without its marker, or a normal pair retaining an old marker.
inline bool syntheticMarkerConsistent(bool synthetic, bool markerPresent)
{
	return synthetic == markerPresent;
}

// Publication-side form of the same invariant: the marker write must have
// succeeded AND the resulting on-disk marker must match the pair's provenance.
// Either failure leaves an interrupted transition, so the caller restores the
// previous pair and marker instead of publishing half of one.
inline bool syntheticMarkerPublicationConsistent(bool synthetic,
                                                 bool operationSucceeded,
                                                 bool markerPresent)
{
	return operationSucceeded && syntheticMarkerConsistent(synthetic, markerPresent);
}

// Metadata written before the explicit provenance field existed is still
// readable: back then `synthetic_<appid>` itself was the persisted provenance
// bit, so a legacy pair is valid with or without that marker. Explicit records
// must agree with their marker.
inline bool syntheticMarkerStateConsistent(bool hasSyntheticMetadata,
                                            bool synthetic,
                                            bool markerPresent)
{
	// Legacy normal (no marker) and legacy synthetic (marker present) pairs
	// are both valid, so provenance cannot be inferred and is not required.
	if (!hasSyntheticMetadata) return true;
	return syntheticMarkerConsistent(synthetic, markerPresent);
}

// A managed-source removal may retain only the synthetic marker when the app
// remains active through compatibility ownership. That marker is a durable
// PICS-protection state, not a readable cache pair; it must remain effective
// after a process restart while the metadata file is absent.
inline bool retainedSyntheticMarkerProtectionAllowed(
    bool markerPresent, bool metadataPresent, bool activeCompatibility)
{
	return markerPresent && !metadataPresent && activeCompatibility;
}

// Preserve the PICS protection marker only while compatibility ownership is
// retained and the pair is known to be synthetic. An explicit normal pair
// with a stale marker must not be promoted into synthetic state.
inline bool shouldPreserveSyntheticMarker(bool retainCompatibility,
                                           bool isSynthetic)
{
	return retainCompatibility && isSynthetic;
}

// A child-metadata sidecar belongs to the base pair that was current before
// publication.  Preserve it when the pair transaction rolls back; once the
// new pair commits, readers must either observe matching new metadata or
// reject the old sidecar by its base digest.
inline bool shouldInvalidateDlcMetadata(bool basePublicationSucceeded)
{
	return basePublicationSucceeded;
}

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

// Full validation is needed only on paths that can serve the buffer: a fresh
// same-boot hit or an offline fallback.  A stale online buffer is going to be
// refreshed and must not pay the YAML/SHA-1/VDF validation cost.
inline bool shouldValidateCache(bool cacheFresh, bool refreshUnavailable)
{
	return cacheFresh || refreshUnavailable;
}

inline bool wireSizeMatches(unsigned long long actualSize,
                            unsigned long long declaredSize)
{
	return declaredSize > 0 && actualSize == declaredSize;
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
