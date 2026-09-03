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

#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/provision_cache.hpp"
#include "../src/feats/provision_schedule.hpp"

#include <cstdio>
#include <map>
#include <optional>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using AppInfoProvision::CacheReadiness;
	using AppInfoProvision::coldFallbackNeeded;
	using AppInfoProvision::cache::CacheUse;
	using AppInfoProvision::cache::CacheMetadataView;
	using AppInfoProvision::cache::CacheRecordFacts;
	using AppInfoProvision::cache::CacheValidationKey;
	using AppInfoProvision::cache::cachePublicationAllowed;
	using AppInfoProvision::cache::chooseCacheUse;
	using AppInfoProvision::cache::protonPublicationAllowed;
	using AppInfoProvision::cache::parseCacheMetadata;
	using AppInfoProvision::cache::syntheticMarkerConsistent;
	using AppInfoProvision::cache::syntheticMarkerPublicationConsistent;
	using AppInfoProvision::cache::syntheticMarkerStateConsistent;
	using AppInfoProvision::cache::retainedSyntheticMarkerProtectionAllowed;
	using AppInfoProvision::cache::isBufferReusable;
	using AppInfoProvision::cache::isCacheRecordValid;
	using AppInfoProvision::cache::locallyAuthoritative;
	using AppInfoProvision::cache::shouldValidateCache;
	using AppInfoProvision::cache::shouldPreserveCacheFromRawPics;
	using AppInfoProvision::cache::shouldPreserveSyntheticMarker;
	using AppInfoProvision::cache::shouldInvalidateDlcMetadata;
	using AppInfoProvision::cache::wireSizeMatches;

	CHECK(!coldFallbackNeeded(CacheReadiness::Busy),
	      "busy runtime cache never becomes synchronous network work");
	CHECK(!coldFallbackNeeded(CacheReadiness::Unverified),
	      "unverified runtime cache defers to background validation");
	CHECK(coldFallbackNeeded(CacheReadiness::Missing),
	      "confirmed runtime miss remains recoverable");
	CHECK(coldFallbackNeeded(CacheReadiness::Invalid),
	      "confirmed invalid runtime pair remains recoverable");
	CHECK(!coldFallbackNeeded(CacheReadiness::ValidStale),
	      "validated stale cache remains usable at startup");

	CacheMetadataView metadata{};
	CHECK(!parseCacheMetadata("", metadata),
	      "missing cache metadata is rejected without a parser exception");
	CHECK(!parseCacheMetadata("synthetic: false\n", metadata),
	      "incomplete cache metadata is rejected");
	CHECK(!parseCacheMetadata(
	          "appid: 3405340\nchange_number: 1\nwire_size: 8192\n"
	          "sha_b64: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\nnormalized: true\n"
	          "synthetic: false\nbroken yaml\n",
	          metadata),
	      "malformed cache metadata is rejected even with a valid synthetic field");
	CHECK(!parseCacheMetadata(
	          "appid: 3405340\nchange_number: 1\nwire_size: 8192\n"
	          "sha_b64: AAAAAAAAAAAAA=AAAAAAAAAAAAAA\nnormalized: true\n"
	          "synthetic: false\n",
	          metadata),
	      "non-canonical SHA padding is rejected before Base64 decoding");
	CHECK(parseCacheMetadata(
	          "appid: 3405340\nchange_number: 1\nwire_size: 8192\n"
	          "sha_b64: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\nnormalized: true\n"
	          "synthetic: false\n",
	          metadata) &&
	      metadata.appId == 3405340 && metadata.changeNumber == 1 &&
	      metadata.wireSize == 8192 &&
	      metadata.hasNormalized && metadata.normalized &&
	      metadata.hasSynthetic && !metadata.synthetic,
	      "generated cache metadata is parsed without yaml-cpp");
	CHECK(parseCacheMetadata(
	          "appid: 3405340\nchange_number: 2\nwire_size: 4096\n"
	          "sha_b64: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\nnormalized: false\n"
	          "synthetic: false\n",
	          metadata) && metadata.hasNormalized && !metadata.normalized,
	      "explicit raw cache provenance is retained by the no-throw parser");
	CHECK(parseCacheMetadata(
	          "appid: 3405340\nchange_number: 1\nwire_size: 8192\n"
	          "sha_b64: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\n",
	          metadata) && !metadata.hasNormalized && !metadata.hasSynthetic,
	      "legacy cache metadata without provenance markers remains readable");

	const CacheValidationKey fileIdentity{
	    .appId = 420530,
	    .mtimeSecs = 1'000'000,
	    .mtimeNsecs = 123'000'000,
	    .size = 8192,
	    .inode = 7001,
	};
	CHECK(fileIdentity == fileIdentity,
	      "unchanged appinfo file identity reuses its validation result");
	{
		auto changed = fileIdentity;
		++changed.mtimeNsecs;
		CHECK(!(changed == fileIdentity),
		      "nanosecond mtime change invalidates same-second cache identity");
	}
	{
		auto changed = fileIdentity;
		++changed.inode;
		CHECK(!(changed == fileIdentity),
		      "replacement inode invalidates same-size cache identity");
	}
	{
		auto changed = fileIdentity;
		++changed.size;
		CHECK(!(changed == fileIdentity),
		      "size change invalidates cache identity");
	}
	{
		auto changed = fileIdentity;
		++changed.metadataInode;
		CHECK(!(changed == fileIdentity),
		      "metadata replacement invalidates cache identity");
	}
	{
		auto changed = fileIdentity;
		++changed.metadataMtimeNsecs;
		CHECK(!(changed == fileIdentity),
		      "metadata same-second rewrite invalidates cache identity");
	}
	const auto publishedIdentity =
		AppInfoProvision::validatedPublicationIdentity(
			/*publicationSucceeded=*/true,
			/*validationSucceeded=*/true,
			std::optional<CacheValidationKey>{fileIdentity});
	CHECK(publishedIdentity && *publishedIdentity == fileIdentity,
	      "successful publication exposes its exact post-write identity");
	CHECK(!AppInfoProvision::validatedPublicationIdentity(
			/*publicationSucceeded=*/false,
			/*validationSucceeded=*/true,
			std::optional<CacheValidationKey>{fileIdentity}),
	      "failed publication cannot seed the validation memo");
	CHECK(!AppInfoProvision::validatedPublicationIdentity(
			/*publicationSucceeded=*/true,
			/*validationSucceeded=*/false,
			std::optional<CacheValidationKey>{fileIdentity}),
	      "unvalidated publication cannot seed the validation memo");
	CHECK(!AppInfoProvision::validatedPublicationIdentity(
			/*publicationSucceeded=*/true,
			/*validationSucceeded=*/true, std::nullopt),
	      "publication without a post-write identity cannot seed the memo");
	std::map<CacheValidationKey, bool> publicationMemo;
	CHECK(AppInfoProvision::memoizeValidatedPublication(
			publicationMemo, /*publicationSucceeded=*/true,
			/*validationSucceeded=*/true,
			std::optional<CacheValidationKey>{fileIdentity}, true) &&
	      publicationMemo.size() == 1 && publicationMemo.at(fileIdentity),
	      "successful validation memoizes the exact post-publication identity");
	auto rejectedIdentity = fileIdentity;
	++rejectedIdentity.inode;
	CHECK(!AppInfoProvision::memoizeValidatedPublication(
			publicationMemo, /*publicationSucceeded=*/false,
			/*validationSucceeded=*/true,
			std::optional<CacheValidationKey>{rejectedIdentity}, true) &&
	      publicationMemo.size() == 1,
	      "failed publication leaves the validation memo unchanged");
	CHECK(!AppInfoProvision::memoizeValidatedPublication(
			publicationMemo, /*publicationSucceeded=*/true,
			/*validationSucceeded=*/false,
			std::optional<CacheValidationKey>{rejectedIdentity}, true) &&
	      publicationMemo.size() == 1,
	      "failed validation leaves the validation memo unchanged");

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

	// A stale cache must not pay the expensive YAML/SHA-1/VDF validation
	// while a live refresh is available. It is validated only if the refresh
	// path is unavailable and the stale buffer may actually be served.
	CHECK(!shouldValidateCache(/*cacheFresh=*/false,
	                           /*refreshUnavailable=*/false),
	      "stale online buffer bypasses expensive validation");
	CHECK(shouldValidateCache(false, true),
	      "stale buffer is validated for offline fallback");
	CHECK(shouldValidateCache(true, false),
	      "fresh buffer is validated before it is served");
	CHECK(wireSizeMatches(/*actual=*/8192, /*declared=*/8192),
	      "matching metadata wire size passes the cheap gate");
	CHECK(!wireSizeMatches(8191, 8192),
	      "truncated buffer fails the cheap wire-size gate");

	// Pairs written before provenance metadata was introduced are ambiguous:
	// preserve them from raw PICS replacement until a provider refresh writes
	// the explicit marker.
	CHECK(shouldPreserveCacheFromRawPics(/*hasMarker=*/false,
	                                     /*normalized=*/false),
	      "legacy cache is protected from raw PICS replacement");
	CHECK(shouldPreserveCacheFromRawPics(/*hasMarker=*/true,
	                                     /*normalized=*/true),
	      "normalized cache is protected from raw PICS replacement");
	CHECK(!shouldPreserveCacheFromRawPics(/*hasMarker=*/true,
	                                      /*normalized=*/false),
	      "explicit raw cache remains replaceable by a newer PICS response");
	CHECK(locallyAuthoritative({
	          .managed = true, .active = true, .cacheValid = true,
	          .hasNormalizedMarker = true, .normalized = true}),
	      "validated normalized managed cache is locally authoritative");
	CHECK(locallyAuthoritative({
	          .managed = true, .active = true, .cacheValid = true,
	          .hasNormalizedMarker = false}),
	      "validated legacy managed cache remains locally authoritative");
	CHECK(!locallyAuthoritative({
	          .managed = true, .active = true, .cacheValid = true,
	          .hasNormalizedMarker = true, .normalized = false}),
	      "explicit raw PICS cache remains refreshable");
	CHECK(!locallyAuthoritative({
	          .managed = true, .active = true, .cacheValid = false,
	          .hasNormalizedMarker = true, .normalized = true}),
	      "invalid normalized cache cannot become authoritative");
	CHECK(locallyAuthoritative({
	          .managed = false, .active = true, .synthetic = true}),
	      "active marker-only synthetic app remains authoritative after source removal");
	CHECK(!locallyAuthoritative({
	          .managed = false, .active = false, .synthetic = true}),
	      "full active removal revokes retained synthetic authority");
	CHECK(!locallyAuthoritative({
	          .managed = true, .active = true, .synthetic = false,
	          .cacheValid = false}),
	      "re-added app cannot inherit stale authority without current evidence");
	CHECK(cachePublicationAllowed(/*managed=*/true,
	                              /*expectedGeneration=*/7,
	                              /*currentGeneration=*/7),
	      "current managed generation may publish a cache pair");
	CHECK(!cachePublicationAllowed(/*managed=*/true,
	                               /*expectedGeneration=*/7,
	                               /*currentGeneration=*/8),
	      "removed and re-added app rejects an old cache publication");
	CHECK(!cachePublicationAllowed(/*managed=*/false,
	                               /*expectedGeneration=*/7,
	                               /*currentGeneration=*/7),
	      "removed app cannot publish even when generations match");
	CHECK(protonPublicationAllowed(/*managed=*/true,
	                              /*expectedGeneration=*/7,
	                              /*currentGeneration=*/7),
	      "current managed generation may mark Proton as needed");
	CHECK(!protonPublicationAllowed(/*managed=*/true,
	                               /*expectedGeneration=*/7,
	                               /*currentGeneration=*/8),
	      "removed and re-added app rejects an old Proton mark");
	CHECK(!protonPublicationAllowed(/*managed=*/false,
	                               /*expectedGeneration=*/7,
	                               /*currentGeneration=*/7),
	      "removed app cannot leave a Proton mark behind");
	// A terminal content verdict must be remembered so the app stops paying a
	// CM round-trip per pass, and must expire when the generation changes so a
	// removed-and-re-added app is retried once.
	CHECK(AppInfoProvision::cache::terminalResultStillApplies(
	          /*recorded=*/true, /*recordedGeneration=*/4,
	          /*currentGeneration=*/4),
	      "a terminal verdict from the current generation still applies");
	CHECK(!AppInfoProvision::cache::terminalResultStillApplies(
	          /*recorded=*/true, /*recordedGeneration=*/4,
	          /*currentGeneration=*/5),
	      "re-adding the app retries a previously terminal verdict");
	CHECK(!AppInfoProvision::cache::terminalResultStillApplies(
	          /*recorded=*/false, /*recordedGeneration=*/0,
	          /*currentGeneration=*/0),
	      "an app with no recorded verdict is never skipped");

	CHECK(syntheticMarkerConsistent(/*synthetic=*/false,
	                                /*markerPresent=*/false),
	      "a normal cache pair without a marker is consistent");
	CHECK(syntheticMarkerConsistent(/*synthetic=*/true,
	                                /*markerPresent=*/true),
	      "a synthetic cache pair with its marker is consistent");
	CHECK(!syntheticMarkerConsistent(/*synthetic=*/false,
	                                 /*markerPresent=*/true),
	      "a normal cache pair with a stale marker is rejected");
	CHECK(!syntheticMarkerConsistent(/*synthetic=*/true,
	                                 /*markerPresent=*/false),
	      "a synthetic cache pair without a marker is inconsistent");
	CHECK(syntheticMarkerPublicationConsistent(
	          /*synthetic=*/false, /*operationSucceeded=*/true,
	          /*markerPresent=*/false),
	      "successful normal-marker removal publishes a marker-free pair");
	CHECK(!syntheticMarkerPublicationConsistent(
	           /*synthetic=*/false, /*operationSucceeded=*/false,
	           /*markerPresent=*/false),
	      "failed normal-marker removal rejects the cache publication");
	CHECK(!syntheticMarkerPublicationConsistent(
	           /*synthetic=*/true, /*operationSucceeded=*/true,
	           /*markerPresent=*/false),
	      "a synthetic publication whose marker vanished is rejected");
	CHECK(syntheticMarkerStateConsistent(/*hasSyntheticMetadata=*/false,
	                                     /*synthetic=*/false,
	                                     /*markerPresent=*/false),
	      "legacy metadata without a marker is treated as normal");
	CHECK(syntheticMarkerStateConsistent(/*hasSyntheticMetadata=*/false,
	                                     /*synthetic=*/false,
	                                     /*markerPresent=*/true),
	      "legacy metadata with its historical synthetic marker remains readable");
	CHECK(syntheticMarkerStateConsistent(/*hasSyntheticMetadata=*/true,
	                                     /*synthetic=*/true,
	                                     /*markerPresent=*/true),
	      "explicit synthetic metadata still requires its marker");
	CHECK(retainedSyntheticMarkerProtectionAllowed(
	          /*markerPresent=*/true, /*metadataPresent=*/false,
	          /*activeCompatibility=*/true),
	      "a retained marker protects an active compatibility app after restart");
	CHECK(!retainedSyntheticMarkerProtectionAllowed(
	           /*markerPresent=*/true, /*metadataPresent=*/false,
	           /*activeCompatibility=*/false),
	      "a retained marker does not protect an inactive app");
	CHECK(!retainedSyntheticMarkerProtectionAllowed(
	           /*markerPresent=*/true, /*metadataPresent=*/true,
	           /*activeCompatibility=*/true),
	      "a marker beside metadata is not the retained-marker state");
	CHECK(shouldPreserveSyntheticMarker(/*retainCompatibility=*/true,
	                                    /*isSynthetic=*/true),
	      "managed-only cleanup preserves a confirmed synthetic marker");
	CHECK(!shouldPreserveSyntheticMarker(/*retainCompatibility=*/true,
	                                     /*isSynthetic=*/false),
	      "managed-only cleanup removes a stale normal marker");
	CHECK(!shouldPreserveSyntheticMarker(/*retainCompatibility=*/false,
	                                     /*isSynthetic=*/true),
	      "full cleanup removes even a confirmed synthetic marker");
	CHECK(!shouldInvalidateDlcMetadata(/*basePublicationSucceeded=*/false),
	      "failed base publication preserves compatible DLC metadata");
	CHECK(shouldInvalidateDlcMetadata(/*basePublicationSucceeded=*/true),
	      "committed base publication invalidates its prior DLC metadata");

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
