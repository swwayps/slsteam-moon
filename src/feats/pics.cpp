
#include "pics.hpp"

#include "appinfo_provision.hpp"
#include "appinfostate.hpp"
#include "provision_cache.hpp"
#include "appinfo_vdf.hpp"
#include "hotreload.hpp"
#include "synthmark.hpp"
#include "apps.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../thread_start.hpp"

#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
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
	std::error_code ec;
	if (!std::filesystem::exists(dir, ec))
		std::filesystem::create_directories(dir, ec);
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

// Package 0 is owned by the HotReload snapshot; a published cache only widens
// the local DLC set used by launch-time checks.
void refreshDiscoveredDlcAfterCachePublication()
{
	std::lock_guard<std::mutex> passLock(
		AppInfoProvision::provisioningPassMutex());
	bool complete = false;
	const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps(&complete);
	if (complete) Apps::setDiscoveredAppDlcIds(dlcIds.appDlc);
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
			try { refreshDiscoveredDlcAfterCachePublication(); }
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

// Queue this answer's refreshes with the caches found missing at startup,
// then the DLC metadata repair, which runs last so an older library entry
// never delays the game that triggered the callback (it has its own cooldown).
void queueCallbackRepairs(const std::string& appinfoVdfPath,
                          std::vector<AppInfoProvision::RefreshRequest> requests)
{
	const auto startupCacheRepairs = HotReload::takeMissingCacheRepairRequests();
	requests.insert(requests.end(), startupCacheRepairs.begin(), startupCacheRepairs.end());
	if (!requests.empty())
		AppInfoProvision::refreshInBackground(appinfoVdfPath, requests);
	HotReload::repairMissingDlcMetadata(appinfoVdfPath);
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

	g_pLog->debugOnce
	(
		"PICS: response apps=%d packages=%d unknown_apps=%d unknown_packages=%d meta_only=%i\n",
		resp->apps_size(),
		resp->packages_size(),
		resp->unknown_appids_size(),
		resp->unknown_packageids_size(),
		resp->meta_data_only() ? 1 : 0
	);

	// Observe and queue only. Missing/invalid pairs are repaired by detached
	// workers below; no provider, cache write, or filesystem scan may block
	// Steam's product-info receive thread.
	const std::string appinfoVdfPath = AppInfoVdf::findExistingPath();
	std::vector<RawCacheItem> rawCacheItems;
	std::unordered_set<uint32_t> rawCacheRepairs;

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
			// Never edit app->buffer(): Steam checks it against the SHA-1 from
			// the PICS changelist, so an edited buffer is "corrupt data" and
			// Steam re-requests product info forever ("Loading user data").
			// Manifest pins apply when Steam plans the install (manifestbind).
			// Copy the unmodified response for off-thread persistence only when
			// it repairs a confirmed miss.
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
	queueCallbackRepairs(appinfoVdfPath, excludeRefreshRequestsForApps(
		plan.requests,
		rawCacheWorkerAvailable ? rawCacheRepairs
		                        : std::unordered_set<uint32_t>{}));
}

void recvUnmanagedProductInfoResponse()
{
	queueCallbackRepairs(AppInfoVdf::findExistingPath(), {});
}

void recvChangesSinceResponse(CMsgClientPICSChangesSinceResponse* resp)
{
	if (!resp) return;

	const auto managed = g_config.managedAppIds.get();
	std::vector<AppInfoProvision::ObservedAppState> managedChanges;
	std::unordered_set<uint32_t> suppressedManagedApps;
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
		// A retained synthetic compatibility row can be authoritative without
		// remaining provider-managed. The published store keeps this callback
		// free of cache I/O; only managed rows are queued for provider refresh.
		if (AppInfoState::isAuthoritative(appId))
		{
			if (managed.count(appId) != 0)
				suppressedManagedApps.insert(appId);
			g_pLog->debug("PICS: stripping locally authoritative app %u from changelist\n",
			              appId);
			resp->mutable_app_changes()->DeleteSubrange(i, 1);
			++stripped;
		}
	}
	if (stripped > 0)
	{
		g_pLog->info("PICS: filtered %d locally authoritative app(s) from changelist (%d remaining)\n",
		             stripped, resp->app_changes_size());
	}

	std::vector<AppInfoProvision::RefreshRequest> refreshRequests;
	if (resp->force_full_app_update())
	{
		refreshRequests.reserve(suppressedManagedApps.size());
		for (const uint32_t appId : suppressedManagedApps)
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
			suppressedManagedApps);
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

} // namespace PICS
