
#include "pics.hpp"

#include "appinfo_provision.hpp"
#include "provision_cache.hpp"
#include "appinfo_vdf.hpp"
#include "depotkey.hpp"
#include "hotreload.hpp"
#include "manifeststore.hpp"
#include "prewarm.hpp"
#include "synthmark.hpp"
#include "apps.hpp"
#include "packagepatch.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../ownerwork.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../thread_start.hpp"
#include "../update.hpp"

#include "../utils/ManifestFetch.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PICS
{

namespace
{

std::string getCacheDir()
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
	}
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".bin";
	return ss.str();
}

std::string getMetaPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".yaml";
	return ss.str();
}

// A provider-normalized pair, or a legacy pair with no provenance marker,
// remains authoritative only while its metadata still describes the complete
// binary. Legacy pairs are deliberately preserved until a provider refresh
// rewrites them with an explicit marker; otherwise a raw PICS response could
// silently replace a previously normalized cache.
// This also prevents a failed metadata publication from leaving an old
// normalized=true marker that suppresses raw PICS recovery for a newly
// replaced or truncated buffer.
bool hasNormalizedCache(uint32_t appId)
{
	try
	{
		// This runs on the detached raw-cache worker. Do not invoke yaml-cpp
		// here: its LoadFile error path cannot reliably unwind through the
		// optimized portable build and a concurrently replaced metadata file
		// can therefore abort Steam. The emitted cache format is deliberately
		// small and already has a bounded, no-throw parser.
		std::ifstream metadataInput(getMetaPath(appId),
		                           std::ios::binary | std::ios::ate);
		if (!metadataInput.is_open()) return false;
		const std::streamsize metadataSize = metadataInput.tellg();
		constexpr std::streamsize kMaxMetadataSize = 64 << 10;
		if (metadataSize <= 0 || metadataSize > kMaxMetadataSize) return false;
		std::string metadataText(static_cast<std::size_t>(metadataSize), '\0');
		metadataInput.seekg(0, std::ios::beg);
		if (!metadataInput.read(metadataText.data(), metadataSize) ||
		    metadataInput.gcount() != metadataSize) return false;
		AppInfoProvision::cache::CacheMetadataView metadata;
		if (!AppInfoProvision::cache::parseCacheMetadata(metadataText, metadata) ||
		    metadata.appId != appId)
			return false;
		if (!AppInfoProvision::cacheMarkerAllowsRead(appId))
			return false;
		if (!AppInfoProvision::cache::shouldPreserveCacheFromRawPics(
		        metadata.hasNormalized, metadata.normalized))
			return false;

		const auto declaredSha = std::string(
			base64::from_base64(std::string(metadata.shaBase64)));
		constexpr std::size_t kSha1Size = 20;
		if (declaredSha.size() != kSha1Size)
			return false;

		std::ifstream ifs(getBufferPath(appId),
		                  std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) return false;
		const std::streamsize rawSize = ifs.tellg();
		if (rawSize <= 0 || rawSize > (16LL << 20) ||
		    metadata.wireSize != static_cast<std::size_t>(rawSize))
			return false;
		std::string wire(static_cast<std::size_t>(rawSize), '\0');
		ifs.seekg(0, std::ios::beg);
		if (!ifs.read(wire.data(), rawSize)) return false;

		std::uint8_t digest[kSha1Size]{};
		AppInfoProvision::sha1Bytes(wire.data(), wire.size(), digest);
		return std::memcmp(declaredSha.data(), digest, kSha1Size) == 0;
	}
	catch (...)
	{
		return false;
	}
}

bool persistAppBuffer(uint32_t appId, uint32_t changeNumber,
                      const std::string& sha, const std::string& buffer,
                      const AppInfoProvision::CachePublicationToken& publication)
{
	if (!publication.managed)
	{
		g_pLog->debug("PICS: ignoring stale response for removed app=%u\n", appId);
		return false;
	}
	(void)getCacheDir();
	std::lock_guard<std::mutex> publicationLock(
	    AppInfoProvision::cachePublicationMutex());
	ProcessLock::FileLock cacheLock(AppInfoProvision::cacheLockPath(), false);
	if (!cacheLock.acquired())
	{
		g_pLog->info("PICS: unable to lock cache pair for app=%u\n", appId);
		return false;
	}
	if (!AppInfoProvision::cache::cachePublicationAllowed(
	        publication.managed, publication.generation,
	        AppInfoProvision::cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "PICS: rejecting stale cache publication for app=%u\n", appId);
		return false;
	}
	if (buffer.empty()) return false;
	if (sha.size() != 20)
	{
		g_pLog->debug("PICS: refusing to persist app=%u (bad sha size %zu)\n",
		              appId, sha.size());
		return false;
	}
	if (hasNormalizedCache(appId))
	{
		g_pLog->debug(
		    "PICS: retaining normalized or legacy cache for app=%u\n", appId);
		return false;
	}

	const auto bufPath = getBufferPath(appId);

	YAML::Emitter em;
	em << YAML::BeginMap;
	em << YAML::Key << "appid"          << YAML::Value << appId;
	em << YAML::Key << "change_number"  << YAML::Value << changeNumber;
	em << YAML::Key << "wire_size"      << YAML::Value << buffer.size();
	em << YAML::Key << "sha_b64"        << YAML::Value << base64::to_base64(sha);
	em << YAML::Key << "normalized"     << YAML::Value << false;
	em << YAML::Key << "synthetic"      << YAML::Value << false;
	em << YAML::EndMap;
	const std::string metadata(em.c_str(), em.size());
	const bool markerBefore = SynthMark::isMarked(getCacheDir(), appId);
	std::string writeError;
	if (!AppInfoProvision::publishCachePairLocked(
	        appId, buffer, metadata, /*synthetic=*/false, markerBefore,
	        writeError))
	{
		g_pLog->debug(
		    "PICS: unable to publish cache pair for app=%u: %s\n",
		    appId, writeError.c_str());
		return false;
	}
	AppInfoProvision::clearCacheReadInvalidation(appId);
	if (!AppInfoProvision::memoizePublishedCachePairLocked(appId))
		return false;

	g_pLog->debug("PICS: cached app=%u change=%u buffer=%zu bytes -> %s\n",
	              appId, changeNumber, buffer.size(), bufPath.c_str());
	return true;
}

std::vector<std::pair<uint32_t, uint64_t>> extractDepotsAndGids(const std::string& buf)
{
	// Single definition shared with the background pre-warm worker
	// (feats/prewarm.hpp) so the install path and the warm path mine the
	// provisioned buffer identically.
	return Prewarm::extractDepotsAndGids(buf);
}

// Read a previously-persisted product-info buffer from our cache.
// Used to recover the depot/gid list for AdditionalApps whose live CM
// product-info response carries an empty buffer (see the staging
// fallback in recvProductInfoResponse). AppInfoProvision and
// persistAppBuffer share this `picsbuffer_<appid>.bin` path; provider-
// normalized pairs are marked in metadata and remain authoritative over
// raw responses.
std::string readCachedBuffer(uint32_t appId)
{
	std::string out;
	if (!AppInfoProvision::readValidatedCacheBuffer(appId, out)) return {};
	return out;
}

void cleanShaderHitCache(uint32_t appId)
{
	const char* home = std::getenv("HOME");
	if (!home) return;

	static const char* steamRoots[] = {
		"/.steam/steam",
		"/.steam/debian-installation",
		"/.local/share/Steam",
	};

	const std::string needle = std::to_string(appId) + "_pbuf";

	for (const char* suffix : steamRoots)
	{
		const auto userdataDir = std::string(home) + suffix + "/userdata";
		if (!std::filesystem::exists(userdataDir)) continue;

		std::error_code ec;
		for (auto& entry : std::filesystem::recursive_directory_iterator(userdataDir, ec))
		{
			if (!entry.is_regular_file()) continue;
			const auto fn = entry.path().filename().string();
			if (fn == needle)
			{
				g_pLog->info("PICS: removing shader hit cache: %s\n",
				             entry.path().c_str());
				std::filesystem::remove(entry.path(), ec);
			}
		}
	}
}

struct RawCacheItem
{
	uint32_t appId = 0;
	uint32_t changeNumber = 0;
	std::string sha;
	std::string buffer;
	std::string appinfoVdfPath;
	AppInfoProvision::CachePublicationToken publication;
};

std::mutex g_rawCacheQueueMu;
std::map<uint32_t, RawCacheItem> g_rawCachePending;
bool g_rawCacheWorkerActive = false;

void refreshDlcInjectionAfterCachePublication()
{
	std::lock_guard<std::mutex> passLock(
		AppInfoProvision::provisioningPassMutex());
	bool complete = false;
	const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps(&complete);
	if (!complete) return;
	PackagePatch::setExtraAppIds(dlcIds.package0);
	Apps::setAddedAppDlcIds(dlcIds.appDlc);
	const auto ids = AppInfoProvision::mergePackage0AppIds(
		g_config.addedAppIds.get(), dlcIds.package0);
	if (!ids.empty()) (void)OwnerWork::submitHotAdd(ids);
}

void runRawCacheWorker()
{
	for (;;)
	{
		std::map<uint32_t, RawCacheItem> batch;
		{
			std::lock_guard<std::mutex> lock(g_rawCacheQueueMu);
			if (g_rawCachePending.empty())
			{
				g_rawCacheWorkerActive = false;
				return;
			}
			batch.swap(g_rawCachePending);
		}

		bool publishedAny = false;
		std::map<uint32_t, RawCacheItem> deferred;
		for (auto& [appId, item] : batch)
		{
			(void)appId;
			bool persisted = false;
			try
			{
				persisted = persistAppBuffer(
					item.appId, item.changeNumber, item.sha, item.buffer,
					item.publication);
			}
			catch (...)
			{
				persisted = false;
			}
			if (persisted)
			{
				publishedAny = true;
				try { cleanShaderHitCache(item.appId); }
				catch (...) {}
			}
			else
			{
				bool recoveryHandled = false;
				try
				{
					const auto probe = AppInfoProvision::probeCache(
						item.appId,
						AppInfoProvision::CacheProbeMode::BlockingValidate);
					if (probe.readiness == AppInfoProvision::CacheReadiness::Fresh ||
						probe.readiness == AppInfoProvision::CacheReadiness::ValidStale)
					{
						recoveryHandled = true;
					}
					else
					{
						AppInfoProvision::refreshInBackground(
							item.appinfoVdfPath,
							{{item.appId, item.changeNumber,
							  item.publication.generation,
							  AppInfoProvision::reasonMask(
								  AppInfoProvision::RefreshReason::CacheRepair),
							  true, false}});
						recoveryHandled = true;
					}
				}
				catch (...) {}
				if (!recoveryHandled)
					deferred[item.appId] = std::move(item);
			}
		}
		if (publishedAny)
		{
			try { refreshDlcInjectionAfterCachePublication(); }
			catch (...) {}
		}
		if (!deferred.empty())
		{
			std::lock_guard<std::mutex> lock(g_rawCacheQueueMu);
			const bool hadConcurrentPending = !g_rawCachePending.empty();
			for (auto& [appId, item] : deferred)
			{
				auto found = g_rawCachePending.find(appId);
				if (found == g_rawCachePending.end() ||
					rawCacheItemShouldReplace(
						found->second.publication.generation,
						found->second.changeNumber,
						item.publication.generation, item.changeNumber))
				{
					g_rawCachePending[appId] = std::move(item);
				}
			}
			if (rawCacheWorkerShouldContinueAfterDeferral(hadConcurrentPending))
				continue;
			g_rawCacheWorkerActive = false;
			return;
		}
	}
}

bool enqueueRawCacheItems(std::vector<RawCacheItem> items)
{
	if (items.empty()) return false;
	bool startWorker = false;
	{
		std::lock_guard<std::mutex> lock(g_rawCacheQueueMu);
		for (auto& item : items)
		{
			auto found = g_rawCachePending.find(item.appId);
			if (found == g_rawCachePending.end() ||
				rawCacheItemShouldReplace(
					found->second.publication.generation,
					found->second.changeNumber,
					item.publication.generation,
					item.changeNumber))
			{
				g_rawCachePending[item.appId] = std::move(item);
			}
		}
		if (!g_rawCacheWorkerActive)
		{
			g_rawCacheWorkerActive = true;
			startWorker = true;
		}
	}
	if (!startWorker) return true;

	return ThreadStart::startDetached(
		[] {
			ThreadStart::runGuarded(
				runRawCacheWorker,
				[] {
					std::lock_guard<std::mutex> lock(g_rawCacheQueueMu);
					g_rawCacheWorkerActive = false;
				},
				[] {});
		},
		[] {
			std::lock_guard<std::mutex> lock(g_rawCacheQueueMu);
			g_rawCacheWorkerActive = false;
		});
}

} // namespace

void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp)
{
	if (!resp) return;

	const auto managed = g_config.managedAppIds.get();
	std::vector<uint32_t> responseAppIds;
	responseAppIds.reserve(static_cast<std::size_t>(resp->apps_size()));
	for (int i = 0; i < resp->apps_size(); ++i)
		responseAppIds.push_back(resp->apps(i).appid());
	const auto responseManagedApps =
		selectManagedResponseApps(managed, responseAppIds);

	g_pLog->debug
	(
		"PICS: response apps=%d packages=%d unknown_apps=%d unknown_packages=%d meta_only=%i\n",
		resp->apps_size(),
		resp->packages_size(),
		resp->unknown_appids_size(),
		resp->unknown_packageids_size(),
		resp->meta_data_only() ? 1 : 0
	);

	const bool legacyStaging = legacyManifestStagingEnabled(
	    std::getenv("SLSSTEAM_LEGACY_MANIFEST_STAGING"));
	g_pLog->infoOnce(
	    "PICS: manifest staging mode=%s\n",
	    legacyStaging ? "legacy synchronous + prewarm"
	                  : "event-driven Steam install plan");

	// Observe and queue only. Missing/invalid pairs are repaired by detached
	// workers below; no provider, cache write, or filesystem scan may block
	// Steam's product-info receive thread.
	const std::string appinfoVdfPath = AppInfoVdf::findExistingPath();
	std::vector<RawCacheItem> rawCacheItems;
	std::unordered_set<uint32_t> rawCacheRepairs;

	// Rollback-only collection for the old architecture. In normal operation
	// PICS still persists and supplies product info, but performs no manifest
	// directory scans, network fetches, or waits. BuildDepotDependency stages
	// only the depots Steam selected for the real install plan.
	std::vector<AppDepots> toStage;

	const auto added = g_config.addedAppIds.get();
	for (int i = 0; i < resp->apps_size(); ++i)
	{
		auto* app = resp->mutable_apps(i);
		g_pLog->debug
		(
			"PICS: app=%u change=%u missing_token=%i only_public=%i sha_size=%zu buffer_size=%zu\n",
			app->appid(),
			app->change_number(),
			app->missing_token() ? 1 : 0,
			app->only_public() ? 1 : 0,
			app->sha().size(),
			app->buffer().size()
		);

		if (!added.count(app->appid()))
		{
			continue;
		}

		if (app->buffer().size() > 0)
		{
			// IMPORTANT: do NOT rewrite app->buffer() here.
			//
			// We used to pin manifest GIDs by editing the product-info
			// text buffer (ManifestId::applyToWireBuffer) and then
			// re-stamping app->sha().  Steam, however, validates the
			// product-info buffer against the SHA-1 it received in the
			// PICS *changelist* (the authoritative hash from the prior
			// request stage), not against the sha field in this
			// response.  Any edit to the buffer therefore fails Steam's
			// integrity check:
			//
			//     appinfo_log: "Corrupt data in text buffer for app N"
			//     "UpdatesJob: apps still needs updates, run again"
			//
			// which makes Steam re-request product info forever and
			// hangs the client at "Loading user data" on a cold cache
			// (reproduced; the loop only ever hit the AddedApps whose
			// depots had manifest pins, i.e. the ones we rewrote).
			//
			// Manifest-GID pinning is handled at the download layer
			// instead — ManifestCode's GetManifestRequestCode /
			// BYldRequestDepotManifest hooks redirect the actual
			// manifest request to the pinned gid — so dropping the
			// product-info rewrite loses nothing. Copy the exact unmodified response
			// for off-thread persistence only when it repairs a confirmed miss.
			const auto probe = AppInfoProvision::probeCache(
				app->appid(), AppInfoProvision::CacheProbeMode::NonBlockingMemoOnly);
			if (rawResponseCanRepairCache(true, probe.readiness))
			{
				rawCacheItems.push_back({
					app->appid(), app->change_number(), app->sha(), app->buffer(),
					appinfoVdfPath,
					AppInfoProvision::snapshotCachePublication(app->appid())});
				rawCacheRepairs.insert(app->appid());
			}
		}

		if (!legacyStaging)
		{
			continue;
		}

		// Decide which buffer to mine for depots/gids.  For an
		// AdditionalApp whose product info isn't in the local library,
		// the live CM response carries an EMPTY buffer (no depots), so we
		// fall back to the product-info buffer we provisioned to disk
		// during setup() (picsbuffer_<appid>.bin).
		const bool emptyLiveBuffer = app->buffer().size() == 0;
		const std::string warmBuf =
		    emptyLiveBuffer ? readCachedBuffer(app->appid()) : app->buffer();
		if (warmBuf.empty())
		{
			g_pLog->debug("PICS: app=%u no buffer to stage (live=%zu, no cache)\n",
			              app->appid(), app->buffer().size());
			continue;
		}

		auto depots = extractDepotsAndGids(warmBuf);

		// Archive every depot manifest the zip shipped (all platforms)
		// into the purge-proof ManifestStore while they're still in
		// depotcache — before Steam's post-commit purge removes the
		// non-mounted ones (e.g. the windows depot of a native-linux
		// title).  This is what lets a later offline Proton-switch restore
		// + install the windows depot.  AdditionalApps only.
		if (emptyLiveBuffer)
		{
			std::vector<uint32_t> depotIds;
			depotIds.reserve(depots.size());
			for (const auto& [depotId, gid] : depots) depotIds.push_back(depotId);
			ManifestStore::archiveDepots(depotIds);
		}

		if (!emptyLiveBuffer)
		{
			// Library app: Steam drives its own manifest fetch.  A
			// best-effort async prefetch is harmless but never on the
			// critical path, so don't block the recv thread.
			for (const auto& [depotId, gid] : depots)
			{
				// Only prefetch depots WE manage (LuaTools).  An owned
				// library app's depots are Steam's job — touching them here
				// is needless wudrm traffic for content we don't manage.
				if (!DepotKey::isManagedDepot(depotId))
				{
					continue;
				}
				g_pLog->info("PICS: prefetching manifest for app=%u depot=%u gid=%llu\n",
				             app->appid(), depotId, static_cast<unsigned long long>(gid));
				ManifestFetch::submitManifestBlob(gid, app->appid(), depotId);
			}
			continue;
		}

		// AdditionalApp: defer staging to the concurrent pass below.  The
		// key filter (we only stage depots we hold a key for — staging a
		// blob we can't decrypt just wastes a CDN round-trip) is applied
		// there, in buildSyncStagePlan.
		toStage.push_back({app->appid(), std::move(depots)});
	}

	if (legacyStaging)
	{
		// Legacy rollback: ensure every AdditionalApp depot manifest is staged
		// before this handler returns. Ready targets do no work; misses use
		// bounded concurrency.
		//
		// Why staging must finish before we return (confirmed in testing):
		// clicking Install triggers a fresh PICS product-info request, and
		// Steam cannot begin update *planning* until this response is
		// processed (it's what tells Steam which depots/manifests exist), so
		// this recv handler strictly precedes planning. During planning Steam
		// decides whether to call CDepotDownloadMgr::BYldRequestDepotManifest:
		//   - manifest NOT on disk at planning -> Steam calls BYld -> the
		//     ORIGINAL BYld returns 'Access Denied' -> the attempt is canceled
		//     with "No connection" (only the ~30s auto-retry, which left the
		//     blob on disk, ever recovered);
		//   - manifest ALREADY on disk at planning -> Steam SKIPS BYld and goes
		//     straight to Downloading -> success.
		// So the blobs must be on disk before we return. This runs on a genuine
		// Steam worker thread (the InitFromPacket detour).
		//
		// We used to create one std::async thread for EVERY target, including
		// manifests restoreToDepotcache had already made ready. Large depot
		// graphs (311210 exposes 800+) exhausted the 32-bit Steam process during
		// this callback. The corrected flow:
		//   1. performs only a cheap exact on-disk check here;
		//   2. gives already-staged targets no task and no await at all;
		//   3. sends only missing targets to ManifestFetch's fixed worker pool,
		//      where restore-from-store and network I/O happen off this thread.
		// There is no manifest-count cap. We still await genuinely-missing
		// targets because returning before they reach disk makes Steam plan BYld
		// and fail the first install attempt.
		const auto plan = buildSyncStagePlan(
		    toStage,
		    [](uint32_t depotId)
		    {
		        return !DepotKey::getCachedKey(depotId).key.empty();
		    });

		std::size_t alreadyStaged = 0;
		const auto pending = buildPendingStagePlan(
		    plan,
		    [&](const StageTarget& target)
		    {
		        if (!ManifestStore::isInDepotcache(target.depotId, target.gid))
		        {
		            return false;
		        }
		        ++alreadyStaged;
		        return true;
		    });

		if (!plan.empty())
		{
			g_pLog->info(
			    "PICS: manifest plan targets=%zu already_on_disk=%zu pending=%zu\n",
			    plan.size(), alreadyStaged, pending.size());
		}

		// Pass 1: queue only manifests not already present. The fixed executor
		// restores from ManifestStore first and reaches the CDN only on a miss.
		for (const auto& t : pending)
		{
			g_pLog->info(
			    "PICS: staging manifest for app=%u depot=%u gid=%llu "
			    "(bounded worker)\n",
			    t.appId, t.depotId, static_cast<unsigned long long>(t.gid));
			ManifestFetch::submitManifestBlob(t.gid, t.appId, t.depotId);
		}

		// Pass 2: block only for the missing subset (joins the queued work).
		for (const auto& t : pending)
		{
			const bool staged = ManifestFetch::awaitManifestBlob(
			    t.gid, t.depotId, ManifestFetch::getTimeoutSec());
			if (staged)
			{
				ManifestStore::archiveManifest(t.depotId, t.gid);
				ManifestStore::markPreferredGid(t.depotId, t.gid);
			}
			g_pLog->info(
			    "PICS: manifest staging for app=%u depot=%u gid=%llu -> %s\n",
			    t.appId, t.depotId, static_cast<unsigned long long>(t.gid),
			    staged ? "on disk"
			           : "FAILED (will fall back to BYld retry)");
		}

		// Start the background manifest pre-warm worker now that we're on a
		// real Steam worker thread (post-login PICS recv). ensureStarted() is
		// idempotent, so calling it on every recv is cheap. This is rollback
		// behavior only; event-driven staging never starts the worker.
		Prewarm::ensureStarted();
	}
	const bool rawCacheWorkerAvailable =
		enqueueRawCacheItems(std::move(rawCacheItems));

	if (resp->unknown_appids_size() > 0)
	{
		std::stringstream ss;
		for (int i = 0; i < resp->unknown_appids_size(); ++i)
		{
			if (i) ss << ',';
			ss << resp->unknown_appids(i);
		}
		g_pLog->debug("PICS: unknown_appids=[%s]\n", ss.str().c_str());
	}

	std::vector<AppInfoProvision::ObservedAppState> observed;
	observed.reserve(responseManagedApps.size());
	for (int i = 0; i < resp->apps_size(); ++i)
	{
		const auto& app = resp->apps(i);
		if (responseManagedApps.count(app.appid()) == 0) continue;
		const auto probe = AppInfoProvision::probeCache(
			app.appid(), AppInfoProvision::CacheProbeMode::NonBlockingMemoOnly);
		const auto terminal = AppInfoProvision::observeTerminal(
			app.appid(), app.change_number());
		const auto publication =
			AppInfoProvision::snapshotCachePublication(app.appid());
		observed.push_back({
			app.appid(), publication.generation, probe.changeNumber,
			app.change_number(), probe.readiness, terminal.applies,
			terminal.changeNumber});
	}
	const auto plan = AppInfoProvision::selectRefreshRequests(
		observed, AppInfoProvision::RefreshReason::PicsProductInfo);
	auto refreshRequests = excludeRefreshRequestsForApps(
		plan.requests,
		rawCacheWorkerAvailable ? rawCacheRepairs
		                        : std::unordered_set<uint32_t>{});
	auto startupCacheRepairs = HotReload::takeMissingCacheRepairRequests();
	refreshRequests.insert(
		refreshRequests.end(), startupCacheRepairs.begin(),
		startupCacheRepairs.end());
	if (!refreshRequests.empty())
		AppInfoProvision::refreshInBackground(appinfoVdfPath, refreshRequests);

	// Refresh the safe-mode-hash cache (updates.yaml) off the boot path.
	// init() served it from disk synchronously so Steam's launch never
	// blocks on GitHub; this brings it up to date from a real worker
	// thread, gated by a TTL so we don't fetch on every relaunch.
	Updater::refreshInBackgroundIfStale();
	// Run migration repair only after response-specific hot-add/cache work has
	// been queued, so an older library entry can never delay the game that
	// triggered this callback. The coordinator applies its own cooldown.
	HotReload::repairMissingDlcMetadata(appinfoVdfPath);
}

void recvChangesSinceResponse(CMsgClientPICSChangesSinceResponse* resp)
{
	if (!resp) return;

	const auto managed = g_config.managedAppIds.get();
	std::vector<AppInfoProvision::ObservedAppState> managedChanges;
	std::unordered_set<uint32_t> suppressedSyntheticApps;
	managedChanges.reserve(static_cast<std::size_t>(resp->app_changes_size()));
	for (int i = 0; i < resp->app_changes_size(); ++i)
	{
		const auto& change = resp->app_changes(i);
		if (managed.count(change.appid()) == 0) continue;
		const auto probe = AppInfoProvision::probeCache(
			change.appid(), AppInfoProvision::CacheProbeMode::NonBlockingMemoOnly);
		const auto terminal = AppInfoProvision::observeTerminal(
			change.appid(), change.change_number());
		const auto publication =
			AppInfoProvision::snapshotCachePublication(change.appid());
		managedChanges.push_back({
			change.appid(), publication.generation, probe.changeNumber,
			change.change_number(), probe.readiness, terminal.applies,
			terminal.changeNumber});
	}

	int stripped = 0;
	for (int i = resp->app_changes_size() - 1; i >= 0; --i)
	{
		const uint32_t appId = resp->app_changes(i).appid();
		// Unmanaged changelist rows can never carry one of our synthetic pairs.
		// Avoid publication-lock and marker/metadata I/O for Steam's library.
		if (managed.count(appId) != 0 &&
			AppInfoProvision::isSynthesizedApp(appId))
		{
			suppressedSyntheticApps.insert(appId);
			g_pLog->debug("PICS: stripping synthetic app %u from changelist\n",
			              appId);
			resp->mutable_app_changes()->DeleteSubrange(i, 1);
			++stripped;
		}
	}
	if (stripped > 0)
	{
		g_pLog->info("PICS: filtered %d synthetic app(s) from changelist (%d remaining)\n",
		             stripped, resp->app_changes_size());
	}

	std::vector<AppInfoProvision::RefreshRequest> refreshRequests;
	if (resp->force_full_app_update())
	{
		refreshRequests.reserve(suppressedSyntheticApps.size());
		for (const uint32_t appId : suppressedSyntheticApps)
		{
			const auto publication =
				AppInfoProvision::snapshotCachePublication(appId);
			refreshRequests.push_back({
				appId, resp->current_change_number(), publication.generation,
				AppInfoProvision::reasonMask(
					AppInfoProvision::RefreshReason::ForceFull),
				true, true});
		}
	}
	else
	{
		refreshRequests = markRuntimePublicationForSuppressedApps(
			AppInfoProvision::selectRefreshRequests(
				managedChanges, AppInfoProvision::RefreshReason::PicsChanges).requests,
			suppressedSyntheticApps);
	}
	auto startupCacheRepairs = HotReload::takeMissingCacheRepairRequests();
	refreshRequests.insert(
		refreshRequests.end(), startupCacheRepairs.begin(),
		startupCacheRepairs.end());
	if (!refreshRequests.empty())
	{
		const std::string appinfoVdfPath = AppInfoVdf::findExistingPath();
		AppInfoProvision::refreshInBackground(appinfoVdfPath, refreshRequests);
	}
}

void recvMsg(CProtoBufMsgBase* msg)
{
	if (!msg) return;
	switch (msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_RESPONSE:
			recvProductInfoResponse(msg->getBody<CMsgClientPICSProductInfoResponse>());
			break;
		case EMSG_PICS_CHANGES_RESPONSE:
			recvChangesSinceResponse(msg->getBody<CMsgClientPICSChangesSinceResponse>());
			break;
		default:
			break;
	}
}

} // namespace PICS
