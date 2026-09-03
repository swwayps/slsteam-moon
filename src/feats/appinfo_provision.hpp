// SPDX-License-Identifier: AGPL-3.0-only
//
// Cold-start provisioner for AdditionalApps.
//
// Background
// ----------
// For an AdditionalApp that isn't in the user's library (no ticket / no
// AppToken), Valve's CM responds to PICS product-info with a stripped
// buffer that has no `depots` / `manifests` / `branches` blocks.  Steam's
// downloader then sees "0 depots" and the install dialog reports 0 B and
// finishes without downloading anything (visible in `content_log.txt`:
// `0 mounted depots`, `has no changes, 0 active: 0 target`).
//
// We previously tried to enrich the in-flight PICS buffer.  Steam
// validates the buffer against the SHA-1 it received in the PICS
// changelist, so any rewrite tripped the integrity check and looped the
// cold-cache login forever.
//
// This module addresses that case at a different layer: it pulls a
// complete product-info text buffer from a public mirror and splices
// a synthetic entry into `appcache/appinfo.vdf` *before Steam opens the
// file*.  Steam then reads its own cache, finds the depots/manifests,
// and the downloader proceeds normally.  The runtime PICS path is left
// untouched, so the cold-loop fix is preserved.
//
// Provider: `https://api.steamcmd.net/v1/info/{appid}` (Pavel/SteamDB,
// JSON; identical schema to the SteamCMD `app_info_print`, including
// `_change_number` and `_sha`).  Selectable via
// `AppInfoProvision::setProvider` if a future config knob is wired up.
//
// Output writes through the same on-disk cache layout that
// `feats/pics.cpp` uses (`<config>/cache/picsbuffer_<appid>.bin` plus
// `picsbuffer_<appid>.yaml`).  During setup, missing pairs are provisioned
// before `AppInfoVdf::injectAllCached` splices them into the appinfo file;
// background refreshes for already-known apps are consumed on the next
// Steam start.
//
// Idempotent and best-effort: any network/parse failure is logged and
// skipped — Steam continues with whatever it has.

#pragma once

#include "dlcids.hpp"
#include "dlc_metadata.hpp"
#include "manifeststore.hpp"
#include "provision_cache.hpp"
#include "provision_refresh.hpp"
#include "provision_result.hpp"
#include "provision_terminal.hpp"
#include "../config_path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AppInfoProvision
{

enum class CacheProbeMode { BlockingValidate, NonBlockingMemoOnly };

struct CacheProbe
{
	CacheReadiness readiness = CacheReadiness::Missing;
	std::uint32_t changeNumber = 0;
};

CacheProbe probeCache(std::uint32_t appId, CacheProbeMode mode);
// Observation-only recency used outside Steam callbacks to prioritize bounded
// metadata migration. Zero means the pair changed or could not be observed.
std::int64_t cachePairMtimeSecs(std::uint32_t appId) noexcept;
std::string localContentFingerprint(std::uint32_t appId);
std::string localContentFingerprint(
	std::uint32_t appId,
	const ManifestStore::ArchivedGidIndex& archivedGids);
inline std::string fingerprintIndexedLocalInputs(
	std::vector<ProvisionTerminal::LocalInput> inputs,
	const ManifestStore::ArchivedGidIndex& archivedGids)
{
	for (auto& input : inputs)
	{
		const auto found = archivedGids.find(input.depotId);
		input.manifestGid = found == archivedGids.end() ? 0 : found->second;
	}
	return ProvisionTerminal::fingerprint(std::move(inputs));
}

inline std::optional<cache::CacheValidationKey> validatedPublicationIdentity(
	bool publicationSucceeded,
	bool validationSucceeded,
	std::optional<cache::CacheValidationKey> postPublicationIdentity) noexcept
{
	if (!publicationSucceeded || !validationSucceeded ||
		!postPublicationIdentity)
	{
		return std::nullopt;
	}
	return postPublicationIdentity;
}

template <typename Memo, typename Value>
inline bool memoizeValidatedPublication(
	Memo& memo,
	bool publicationSucceeded,
	bool validationSucceeded,
	std::optional<cache::CacheValidationKey> postPublicationIdentity,
	Value&& value)
{
	const auto identity = validatedPublicationIdentity(
		publicationSucceeded, validationSucceeded,
		std::move(postPublicationIdentity));
	if (!identity) return false;
	memo[*identity] = std::forward<Value>(value);
	return true;
}

// Populate the process-local terminal memo from a persisted sidecar.  This is
// intended for startup and watcher/worker contexts; PICS callbacks use only
// the already-populated memo through observeTerminal().
void primeTerminalMemo(std::uint32_t appId, std::string_view fingerprint);
void primeTerminalMemo(std::uint32_t appId);

inline bool requestNeedsFetch(const RefreshRequest& request,
	CacheReadiness readiness,
	std::uint32_t cachedChangeNumber) noexcept
{
	if (request.appId == 0) return false;
	const std::uint8_t nonMetadataReasons = static_cast<std::uint8_t>(
		request.reasons & ~reasonMask(RefreshReason::DlcMetadata));
	if (nonMetadataReasons == 0) return false;
	if (request.forceRefresh) return true;
	if (readiness == CacheReadiness::Missing ||
	    readiness == CacheReadiness::Invalid ||
	    readiness == CacheReadiness::Unverified ||
	    readiness == CacheReadiness::Busy)
		return true;
	return request.minimumChangeNumber > cachedChangeNumber;
}

inline bool cachedRuntimePublicationAllowed(
	const RefreshRequest& request,
	CacheReadiness readiness) noexcept
{
	if (!request.publishRuntime || request.appId == 0) return false;
	const std::uint8_t nonMetadataReasons = static_cast<std::uint8_t>(
		request.reasons & ~reasonMask(RefreshReason::DlcMetadata));
	if (nonMetadataReasons == 0) return false;
	return readiness == CacheReadiness::Fresh ||
		readiness == CacheReadiness::ValidStale;
}

enum class ColdStartMode { StartupRequireUsablePair, RuntimeMissingOnly };

inline bool coldStartPrimesTerminalMemo(ColdStartMode mode) noexcept
{
	return mode == ColdStartMode::StartupRequireUsablePair;
}

inline std::unordered_set<std::uint32_t> selectColdStartApps(
	const std::vector<std::pair<std::uint32_t, CacheReadiness>>& candidates,
	ColdStartMode /*mode*/)
{
	std::unordered_set<std::uint32_t> selected;
	for (const auto& [appId, readiness] : candidates)
	{
		if (appId == 0) continue;
		const bool recoveryNeeded = readiness == CacheReadiness::Missing ||
			readiness == CacheReadiness::Invalid;
		if (recoveryNeeded)
			selected.insert(appId);
	}
	return selected;
}

struct TerminalObservation
{
	bool applies = false;
	std::uint32_t changeNumber = 0;
	ProvisionOutcome outcome = ProvisionOutcome::IncompleteContent;
};

TerminalObservation observeTerminal(
	std::uint32_t appId, std::uint32_t observedChangeNumber);

// Serialize state snapshots, Proton-state transitions, and commit sections.
// CM/provider I/O, retry sleeps, and cache-pair writes must not run while this
// mutex is held; cache writers still take cacheLockPath() for cross-process
// file exclusion.
std::mutex& provisioningPassMutex();

// Compute the 20-byte SHA-1 digest through the same dynamically resolved
// libcrypto helper used by the provisioning paths. The plugin deliberately
// does not link libcrypto directly because Steam supplies the runtime.
void sha1Bytes(const void* data, std::size_t size, std::uint8_t out[20]);

// Cache publication tokens invalidate provider/PICS work that started before
// an app was removed. The publication mutex is also held across quarantine so
// a late writer cannot recreate a pair after cleanup; callers of
// cachePublicationGenerationLocked() must already hold that mutex.
struct CachePublicationToken
{
	bool managed = false;
	std::uint64_t generation = 0;
};

std::mutex& cachePublicationMutex();
std::uint64_t cachePublicationGenerationLocked(uint32_t appId);
CachePublicationToken snapshotCachePublication(uint32_t appId);

// Collect the DLC appids advertised by every managed app from the on-disk
// `picsbuffer_<appid>.bin` buffers.  `package0` contains the planner-facing
// subset: every depot-tagged id plus advertised ids with known own content
// (or all advertised ids when InjectAllAdvertisedDlc is enabled).  `appDlc`
// retains the complete deduplicated set for local launch-time decisions.
// `complete` is false when the cache lock prevents a consistent snapshot or
// any managed buffer is missing, oversized, seek-failed, truncated, or
// otherwise unreadable; callers must retain their previous injection state in
// every incomplete case.
DlcInjectionIds collectDlcAppIdsForAddedApps(bool* complete = nullptr);

// Read a durable metadata-only child cache for one currently managed base.
// `expectedGeneration` closes remove/re-add races. A zero token asks the
// reader to compare against the current process generation: generation zero
// may reuse a cross-process record after base identity validation, while a
// nonzero current generation still requires an exact record match.
bool readValidatedDlcMetadataCache(
	std::uint32_t baseAppId,
	std::uint64_t expectedGeneration,
	DlcMetadata::CacheRecord& record);

// Fetch and persist a synthetic PICS buffer for `appId` if needed.
// `appinfoVdfPath` is the path to Steam's appcache/appinfo.vdf and is
// used to skip apps that already have a usable entry.  Returns true if
// a new buffer was written (or already cached).
bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath);

// Run `provisionApp` only for managed ids sourced from stplug-in or
// luaappids.yaml. Installed compatibility ids never enter provider calls.
// Returns the number of buffers newly written (0 means everything was
// already provisioned or none needed).  `allowConfigWrite` is true only for
// the preinit pass, before Steam can concurrently rewrite config.vdf.
int provisionAllAddedApps(const std::string& appinfoVdfPath,
                          bool allowConfigWrite = true);

struct ProvisionPassSummary
{
	std::size_t requested = 0;
	std::size_t fetched = 0;
	std::size_t updated = 0;
	std::size_t ready = 0;
	std::size_t fallback = 0;
	std::size_t terminal = 0;
	std::size_t failed = 0;
};

ProvisionPassSummary provisionRequestedApps(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requests,
	bool allowConfigWrite = false);

// Apply Proton mappings deferred by runtime provisioning. This is called from
// setup() before Steam starts its live ConfigStore writers.
void flushPendingProtonMappings();

// True when the validated local topology has no native Linux depot and must
// receive a live compatibility mapping before package publication.
bool requiresProton(std::uint32_t appId) noexcept;

// Provision managed apps whose cache pair is not ready for the current PICS
// response. This synchronous fallback runs on the PICS recv thread when
// called at runtime, or during setup() before the appinfo splice when
// `allowConfigWrite` is true. It publishes a normalized cache pair;
// `sanitizedApps`, when supplied, receives ids successfully normalized during
// this invocation. Offline fallback bookkeeping stays pass-local: startup
// splicing independently revalidates every complete pair before merging it.
int provisionColdStartApps(
    const std::string& appinfoVdfPath,
    const std::unordered_set<std::uint32_t>& candidates,
    ColdStartMode mode,
    std::unordered_set<uint32_t>* sanitizedApps = nullptr,
    bool allowConfigWrite = false);

// Resolve the config/env gate for the asynchronous refresh path.
bool asyncProvisioningEnabled();

// Start one detached, AppID-scoped refresh pass from a post-setup worker
// context, such as the PICS receive path or the config watcher. Never call it
// from setup() or the LD_AUDIT la_preinit path. The request vector controls
// exactly which managed generations may be fetched and published.
void refreshInBackground(
    const std::string& appinfoVdfPath,
    const std::vector<RefreshRequest>& requests);

// Common lock held while the cache's .bin/.yaml pair is read or published.
// Keep this inline because AppInfoVdf's standalone transaction test links
// without the provisioner object itself.
inline std::string cacheLockPath()
{
	return ConfigPath::slsteamConfigDir(
	           std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME")) +
	       "/cache/.picsbuffer.lock";
}

// Forget the on-disk app-scoped state for an app that was removed from
// LuaTools. Artifacts are quarantined with a recoverable suffix rather than
// deleted, so a mistaken removal can be restored without data loss. Returns
// false for an invalid app id or when any app-scoped artifact could not be
// quarantined; a missing artifact is a successful no-op.
bool forgetApp(uint32_t appId);

// Forget app-scoped artifacts while retaining ownership ticket files and
// synthetic-PICS protection for an id that remains active through the
// compatibility set.
bool forgetManagedSourceApp(uint32_t appId);

// True iff the cache metadata and synthetic marker describe the same
// explicit publication state, or a marker-only retention state protects a
// still-active compatibility app after managed-source cleanup, and the app
// has not been invalidated for cache reads in this process. Direct cache
// readers use this before consuming a pair without going through
// cacheUseForApp().
bool cacheMarkerAllowsRead(uint32_t appId);

// Publish a complete bin/yaml pair and its synthetic marker. Callers must hold
// cachePublicationMutex() and cacheLockPath() before entering this helper;
// every post-write failure restores the previous pair and marker state.
bool publishCachePairLocked(uint32_t appId, const std::string& wire,
                            const std::string& metadata, bool synthetic,
                            bool markerBefore, std::string& error);

// Validate a just-published pair and memoize its exact bin+yaml identity.
// Callers hold cachePublicationMutex() and cacheLockPath().
bool memoizePublishedCachePairLocked(uint32_t appId);

// Read a cache pair only after validating metadata, SHA-1, VDF structure and
// usable depot content. The helper acquires the cross-process cache lock and
// is the single safe entry point for direct buffer consumers.
bool readValidatedCacheBuffer(uint32_t appId, std::string& buffer);

// Return active appinfo that must remain authoritative over Steam's token-
// gated refresh. Synthetic marker-only compatibility state is retained even
// outside the managed provider scope; normal pairs must be managed, pass full
// cache validation and carry provider-normalized (or legacy ambiguous)
// provenance. Explicit raw-PICS pairs remain refreshable.
std::unordered_set<std::uint32_t> locallyAuthoritativeApps(
	const std::unordered_set<std::uint32_t>& managedCandidates,
	const std::unordered_set<std::uint32_t>& activeCandidates);

// Allow a newly published pair to become readable after a prior managed-app
// removal invalidated the old cache in this process.
void clearCacheReadInvalidation(uint32_t appId);

// True iff `appId`'s appinfo depots were SYNTHESIZED from local manifests
// because its product-info is token-locked (access token denied -> empty
// PICS buffer), with either a consistent cache marker or a retained marker
// protecting an active compatibility app after managed-source cleanup.
// Persisted across the setup() re-exec storm. The outgoing PICS hook
// (apps.cpp::sendPICSInfoRequest) strips these from Steam's product-info
// request so a later empty refresh can't clobber the appinfo we spliced at
// startup (otherwise: install dialog -> 0 B / "Invalid install path").
bool isSynthesizedApp(uint32_t appId);

} // namespace AppInfoProvision
