// SPDX-License-Identifier: AGPL-3.0-only

#include "hotreload.hpp"

#include "appinfo_provision.hpp"
#include "appinfo_vdf.hpp"
#include "appinfostate.hpp"
#include "hotreload_inputs.hpp"
#include "hotreload_publish_policy.hpp"
#include "libraryremoval.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../ownerwork.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct CoordinatorState
{
	std::mutex mutex;
	bool initialized = false;
	std::uint64_t generation = 0;
	std::unordered_set<std::uint32_t> managedAppIds;
	std::unordered_set<std::uint32_t> readyBaseIds;
	std::unordered_map<std::uint32_t, std::string> contentFingerprints;
	std::unordered_map<std::uint32_t, std::uint64_t> managedGenerations;
	std::unordered_map<std::uint32_t, std::int64_t> cacheMtimeSecs;
	std::unordered_set<std::uint32_t> metadataPendingBaseIds;
	std::unordered_set<std::uint32_t> metadataDeferredBaseIds;
	std::vector<std::uint32_t> cacheRepairBaseIds;
	std::chrono::steady_clock::time_point cacheRepairAfter{};
	std::vector<std::uint32_t> metadataRepairBaseIds;
	std::chrono::steady_clock::time_point metadataRepairAfter{};
	bool metadataRepairOpportunitySeen = false;
	PackageSnapshot lastSnapshot;
	bool hasLastSnapshot = false;
};

CoordinatorState& coordinator()
{
	static CoordinatorState state;
	return state;
}

HotReloadState::Store& membershipStore()
{
	// AppInfoState retains a non-owning pointer to this object.  Function-static
	// lifetime keeps it valid through hook teardown and any retained detour.
	static HotReloadState::Store state;
	return state;
}

std::unordered_set<std::uint32_t> publishAppInfoScopes(
	const std::unordered_set<std::uint32_t>& managedAppIds,
	const std::vector<std::uint32_t>& plannerAppIds)
{
	const auto authoritative = AppInfoProvision::locallyAuthoritativeApps(
		managedAppIds, g_config.addedAppIds.get());
	const auto guardedAppIds = HotReloadPublishPolicy::guardedAppInfoIds(
		managedAppIds, plannerAppIds, authoritative);
	// Publish authority first: a newly guarded record must never be observable
	// with a real SHA during the gap before its skip policy becomes active.
	AppInfoState::publishAuthoritative(authoritative);
	(void)membershipStore().publish(guardedAppIds);
	return authoritative;
}

std::vector<std::uint32_t> difference(
	const std::unordered_set<std::uint32_t>& left,
	const std::unordered_set<std::uint32_t>& right)
{
	std::vector<std::uint32_t> sortedLeft(left.begin(), left.end());
	std::vector<std::uint32_t> sortedRight(right.begin(), right.end());
	std::sort(sortedLeft.begin(), sortedLeft.end());
	std::sort(sortedRight.begin(), sortedRight.end());

	std::vector<std::uint32_t> result;
	result.reserve(sortedLeft.size());
	std::set_difference(
		sortedLeft.begin(), sortedLeft.end(),
		sortedRight.begin(), sortedRight.end(),
		std::back_inserter(result));
	return result;
}

bool publishLocked(
	CoordinatorState& state,
	const std::unordered_set<std::uint32_t>& managedAppIds,
	bool force,
	bool allowBackgroundRefresh)
{
	const bool initialPublication = state.generation == 0;
	const bool membershipChanged = managedAppIds != state.managedAppIds;
	if (!HotReloadPublishPolicy::shouldEvaluateInputs(
		initialPublication, membershipChanged, force))
		return false;

	const auto added = difference(managedAppIds, state.managedAppIds);
	const auto removed = difference(state.managedAppIds, managedAppIds);
	const std::uint64_t nextGeneration = state.generation + 1;
	std::unordered_map<std::uint32_t, std::string> nextFingerprints;
	std::vector<std::uint32_t> locallyChanged;
	const auto archivedGids = ManifestStore::archivedGidIndex();
	for (const std::uint32_t appId : managedAppIds)
	{
		const std::string current =
			AppInfoProvision::localContentFingerprint(appId, archivedGids);
		const auto old = state.contentFingerprints.find(appId);
		if (old == state.contentFingerprints.end() || old->second != current)
			locallyChanged.push_back(appId);
		nextFingerprints.emplace(appId, current);
	}
	const bool fingerprintsChanged = !locallyChanged.empty();
	if (!HotReloadPublishPolicy::shouldPublish(
		initialPublication, membershipChanged, fingerprintsChanged))
	{
		return false;
	}
	for (const auto& [appId, fingerprint] : nextFingerprints)
		AppInfoProvision::primeTerminalMemo(appId, fingerprint);
	auto metadataPendingBaseIds = state.metadataPendingBaseIds;
	auto readyBaseIds = state.readyBaseIds;
	if (initialPublication)
		readyBaseIds = managedAppIds;
	for (const std::uint32_t appId : removed)
		readyBaseIds.erase(appId);
	for (const std::uint32_t appId : removed)
		metadataPendingBaseIds.erase(appId);
	for (const std::uint32_t appId : removed)
		state.metadataDeferredBaseIds.erase(appId);
	if (HotReloadPublishPolicy::shouldAwaitDlcMetadata(
		initialPublication, !added.empty()))
		metadataPendingBaseIds.insert(added.begin(), added.end());
	auto built = HotReloadInputs::buildFromCaches(
		nextGeneration, managedAppIds, metadataPendingBaseIds,
		state.metadataDeferredBaseIds, readyBaseIds);
	if (!built.valid)
	{
		if (g_pLog != nullptr)
		{
			g_pLog->warn(
				"HotReload: generation %llu exceeds bounded snapshot capacity; "
				"previous state retained\n",
				static_cast<unsigned long long>(nextGeneration));
		}
		return false;
	}
	built.snapshot.addedAppIds =
		HotReloadPublishPolicy::readyBaseIds(added, readyBaseIds);
	built.snapshot.appInfoRequestIds =
		HotReloadPublishPolicy::membershipAppInfoRequestIds(
			initialPublication, added, built.cacheMissingBaseIds);
	auto cacheRepairBaseIds = state.cacheRepairBaseIds;
	if (initialPublication)
	{
		cacheRepairBaseIds = built.cacheMissingBaseIds;
	}
	else
	{
		const std::unordered_set<std::uint32_t> stillMissing(
			built.cacheMissingBaseIds.begin(), built.cacheMissingBaseIds.end());
		cacheRepairBaseIds.erase(
			std::remove_if(
				cacheRepairBaseIds.begin(), cacheRepairBaseIds.end(),
				[&](std::uint32_t appId)
				{
					return managedAppIds.count(appId) == 0 ||
						stillMissing.count(appId) == 0;
				}),
			cacheRepairBaseIds.end());
	}

	// Guard desired bases immediately, but keep them outside package ownership
	// until their authoritative local appinfo is live.
	const auto authoritative =
		publishAppInfoScopes(managedAppIds, built.snapshot.appIds);
	built.snapshot.appInfoRequestIds =
		HotReloadPublishPolicy::nonAuthoritativeAppInfoRequestIds(
			std::move(built.snapshot.appInfoRequestIds), authoritative);
	for (const std::uint32_t appId : added)
		LibraryRemoval::cancel(appId);

	const OwnerWork::Mode mode =
		OwnerWork::submitManagedState(built.snapshot);
	if (mode == OwnerWork::Mode::Abandoned)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: managed snapshot abandoned during teardown\n");
		return false;
	}

	state.managedAppIds = managedAppIds;
	state.readyBaseIds = std::move(readyBaseIds);
	state.generation = nextGeneration;
	state.contentFingerprints = std::move(nextFingerprints);
	state.metadataPendingBaseIds = std::move(metadataPendingBaseIds);
	state.cacheRepairBaseIds = std::move(cacheRepairBaseIds);
	state.metadataRepairBaseIds = built.metadataMissingBaseIds;
	state.cacheMtimeSecs = built.cacheMtimeSecs;
	for (const std::uint32_t appId : removed)
		state.managedGenerations.erase(appId);
	for (const std::uint32_t appId : managedAppIds)
	{
		if (state.managedGenerations.count(appId) != 0) continue;
		state.managedGenerations[appId] =
			AppInfoProvision::snapshotCachePublication(appId).generation;
	}
	{
		std::vector<HotReloadPublishPolicy::MetadataRepairCandidate> repairs;
		repairs.reserve(state.metadataRepairBaseIds.size());
		for (const std::uint32_t appId : state.metadataRepairBaseIds)
		{
			repairs.push_back({
				appId,
				state.metadataPendingBaseIds.count(appId) != 0,
				state.cacheMtimeSecs[appId],
			});
		}
		state.metadataRepairBaseIds =
			HotReloadPublishPolicy::prioritizeMetadataRepairs(std::move(repairs));
	}
	state.lastSnapshot = built.snapshot;
	state.hasLastSnapshot = true;
	for (const std::uint32_t appId : removed)
		LibraryRemoval::queue(appId);

	if (g_pLog != nullptr)
	{
		g_pLog->info(
			"HotReload: generation %llu dispatched %s (managed=%zu added=%zu "
			"removed=%zu planner_apps=%zu depots=%zu metadata=%s)\n",
			static_cast<unsigned long long>(nextGeneration),
			OwnerWork::modeName(mode), managedAppIds.size(), added.size(),
			removed.size(), built.snapshot.appIds.size(),
			built.snapshot.depotIds.size(),
			built.snapshot.metadataComplete ? "complete" : "pending");
	}

	if (allowBackgroundRefresh && (!added.empty() || !locallyChanged.empty()))
	{
		const std::string appinfoPath = AppInfoVdf::findExistingPath();
		if (!appinfoPath.empty())
		{
			// Best effort only.  Live readiness is driven by the appinfo hook;
			// this detached pass merely publishes a reusable disk cache.
			std::unordered_set<std::uint32_t> addedSet(added.begin(), added.end());
			std::vector<AppInfoProvision::RefreshRequest> requests;
			requests.reserve(added.size() + locallyChanged.size());
			for (const std::uint32_t appId : added)
			{
				const auto publication =
					AppInfoProvision::snapshotCachePublication(appId);
				state.managedGenerations[appId] = publication.generation;
				requests.push_back({appId, 0, publication.generation,
					AppInfoProvision::reasonMask(
						AppInfoProvision::RefreshReason::HotAdd), true, true});
			}
			for (const std::uint32_t appId : locallyChanged)
			{
				if (addedSet.count(appId) != 0) continue;
				const auto publication =
					AppInfoProvision::snapshotCachePublication(appId);
				state.managedGenerations[appId] = publication.generation;
				requests.push_back({appId, 0, publication.generation,
					AppInfoProvision::reasonMask(
						AppInfoProvision::RefreshReason::LocalInputs), true, true});
			}
			AppInfoProvision::refreshInBackground(appinfoPath, requests);
		}
	}

	return true;
}
} // namespace

namespace HotReload
{
HotReloadState::Store& store() noexcept
{
	return membershipStore();
}

void initialize() noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.initialized)
			return;

		// Hooks::setup may already have installed the optional detour against its
		// bootstrap Store. Rebind it before the first runtime snapshot becomes
		// visible; failure remains restart-recoverable in PackagePatch.
		const bool guardReady = AppInfoState::setup(membershipStore());
		state.initialized = true;
		(void)publishLocked(
			state, g_config.managedAppIds.get(), true, false);

		if (!guardReady && g_pLog != nullptr)
		{
			g_pLog->warn(
				"HotReload: appinfo guard unavailable; unresolved additions defer "
				"until restart\n");
		}
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: initialization failed; startup state retained\n");
	}
}

void publish(
	const std::unordered_set<std::uint32_t>& managedAppIds,
	bool forceSourceRefresh) noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.initialized)
			return;
		(void)publishLocked(
			state, managedAppIds, forceSourceRefresh, true);
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: watcher publication failed; previous state retained\n");
	}
}

bool publishPreparedBase(
	std::uint32_t baseAppId,
	std::uint64_t expectedManagedGeneration) noexcept
{
	try
	{
		if (baseAppId == 0) return false;
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> coordinatorLock(state.mutex);
		if (!state.initialized ||
			state.managedAppIds.count(baseAppId) == 0 ||
			state.readyBaseIds.count(baseAppId) != 0) return false;

		// Lock order matches the other completion path. It rejects stale work if
		// the app was removed/re-added while its cache was being prepared.
		std::lock_guard<std::mutex> passLock(
			AppInfoProvision::provisioningPassMutex());
		const auto managed = g_config.managedAppIds.get();
		if (managed != state.managedAppIds ||
			managed.count(baseAppId) == 0) return false;
		{
			std::lock_guard<std::mutex> publicationLock(
				AppInfoProvision::cachePublicationMutex());
			if (AppInfoProvision::cachePublicationGenerationLocked(baseAppId) !=
				expectedManagedGeneration) return false;
		}

		auto readyBaseIds = state.readyBaseIds;
		readyBaseIds.insert(baseAppId);
		const std::uint64_t nextGeneration = state.generation + 1;
		auto built = HotReloadInputs::buildFromCaches(
			nextGeneration, managed, state.metadataPendingBaseIds,
			state.metadataDeferredBaseIds, readyBaseIds);
		if (!built.valid ||
			std::find(built.snapshot.appIds.begin(),
				built.snapshot.appIds.end(), baseAppId) ==
			built.snapshot.appIds.end()) return false;

		built.snapshot.addedAppIds = {baseAppId};
		// This record was just loaded from the normalized local cache. A public
		// request here is redundant and can replace it with an empty record.
		built.snapshot.appInfoRequestIds.clear();

		const auto authoritative =
			publishAppInfoScopes(managed, built.snapshot.appIds);
		built.snapshot.appInfoRequestIds =
			HotReloadPublishPolicy::nonAuthoritativeAppInfoRequestIds(
				std::move(built.snapshot.appInfoRequestIds), authoritative);
		LibraryRemoval::cancel(baseAppId);

		const OwnerWork::Mode mode =
			OwnerWork::submitManagedState(built.snapshot);
		if (mode == OwnerWork::Mode::Abandoned) return false;

		state.readyBaseIds = std::move(readyBaseIds);
		state.generation = nextGeneration;
		state.cacheRepairBaseIds.erase(
			std::remove(state.cacheRepairBaseIds.begin(),
				state.cacheRepairBaseIds.end(), baseAppId),
			state.cacheRepairBaseIds.end());
		if (state.cacheRepairBaseIds.empty()) state.cacheRepairAfter = {};
		state.metadataRepairBaseIds = built.metadataMissingBaseIds;
		state.cacheMtimeSecs = built.cacheMtimeSecs;
		state.lastSnapshot = built.snapshot;
		state.hasLastSnapshot = true;
		if (g_pLog != nullptr)
		{
			g_pLog->info(
				"HotReload: generation %llu published prepared base=%u via %s "
				"(planner_apps=%zu depots=%zu)\n",
				static_cast<unsigned long long>(nextGeneration), baseAppId,
				OwnerWork::modeName(mode), built.snapshot.appIds.size(),
				built.snapshot.depotIds.size());
		}
		return true;
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"HotReload: prepared base publication failed; previous state retained\n");
		return false;
	}
}

void repairMissingDlcMetadata(const std::string& appinfoVdfPath) noexcept
{
	try
	{
		if (appinfoVdfPath.empty()) return;
		CoordinatorState& state = coordinator();
		std::vector<AppInfoProvision::RefreshRequest> requests;
		{
			std::lock_guard<std::mutex> coordinatorLock(state.mutex);
			if (!state.initialized) return;
			const auto now = std::chrono::steady_clock::now();
			const auto nowMs = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					now.time_since_epoch()).count());
			const auto retryAfterMs = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					state.metadataRepairAfter.time_since_epoch()).count());
			if (HotReloadPublishPolicy::shouldArmMetadataRepair(
				state.metadataRepairOpportunitySeen))
			{
				state.metadataRepairOpportunitySeen = true;
				const auto deadlineMs =
					HotReloadPublishPolicy::metadataRepairDeadlineMs(nowMs, 30000);
				state.metadataRepairAfter = std::chrono::steady_clock::time_point(
					std::chrono::milliseconds(deadlineMs));
				return;
			}
			const std::vector<std::uint32_t> pending(
				state.metadataPendingBaseIds.begin(),
				state.metadataPendingBaseIds.end());
			auto repairIds = HotReloadPublishPolicy::mergeMetadataRepairIds(
				state.metadataRepairBaseIds, pending);
			std::vector<HotReloadPublishPolicy::MetadataRepairCandidate> repairs;
			repairs.reserve(repairIds.size());
			for (const std::uint32_t appId : repairIds)
			{
				repairs.push_back({
					appId,
					state.metadataPendingBaseIds.count(appId) != 0,
					state.cacheMtimeSecs[appId],
				});
			}
			repairIds = HotReloadPublishPolicy::prioritizeMetadataRepairs(
				std::move(repairs));
			if (!HotReloadPublishPolicy::metadataRepairDue(
				!repairIds.empty(), nowMs, retryAfterMs)) return;

			// Bound each callback-triggered pass. The shared refresh queue coalesces
			// with hot-add work; remaining bases are retried by later PICS traffic.
			constexpr std::size_t kMaxRepairBases = 1;
			const std::size_t count = std::min(
				kMaxRepairBases, repairIds.size());
			requests.reserve(count);
			for (std::size_t index = 0; index < count; ++index)
			{
				const std::uint32_t appId = repairIds[index];
				const auto found = state.managedGenerations.find(appId);
				if (found == state.managedGenerations.end()) continue;
				const bool runtimePending =
					state.metadataPendingBaseIds.count(appId) != 0;
				if (HotReloadPublishPolicy::metadataRepairDefersUntilRestart(
					runtimePending))
					state.metadataDeferredBaseIds.insert(appId);
				requests.push_back({
					appId, 0, found->second,
					AppInfoProvision::reasonMask(
						AppInfoProvision::RefreshReason::DlcMetadata),
					false, runtimePending});
			}
			state.metadataRepairAfter = now + std::chrono::seconds(30);
		}
		if (!requests.empty())
			AppInfoProvision::refreshInBackground(appinfoVdfPath, requests);
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"HotReload: DLC metadata repair scheduling failed; will retry\n");
	}
}

std::vector<AppInfoProvision::RefreshRequest>
takeMissingCacheRepairRequests() noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> coordinatorLock(state.mutex);
		if (!state.initialized || state.cacheRepairBaseIds.empty()) return {};

		const auto now = std::chrono::steady_clock::now();
		if (now < state.cacheRepairAfter) return {};
		for (std::size_t attempt = 0;
			attempt < state.cacheRepairBaseIds.size(); ++attempt)
		{
			const std::uint32_t appId =
				HotReloadPublishPolicy::takeNextCacheRepairId(
					state.cacheRepairBaseIds);
			const auto found = state.managedGenerations.find(appId);
			if (found == state.managedGenerations.end()) continue;
			state.cacheRepairAfter = now + std::chrono::seconds(30);
			return {{
				appId, 0, found->second,
				AppInfoProvision::reasonMask(
					AppInfoProvision::RefreshReason::CacheRepair),
				true, true,
			}};
		}
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"HotReload: startup cache repair scheduling failed; will retry\n");
	}
	return {};
}

bool noteDlcMetadataCacheCompletion(
	std::uint32_t baseAppId,
	std::uint64_t expectedManagedGeneration) noexcept
{
	try
	{
		if (baseAppId == 0) return false;
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> coordinatorLock(state.mutex);
		if (!state.initialized ||
			state.managedAppIds.count(baseAppId) == 0 ||
			state.metadataPendingBaseIds.count(baseAppId) != 0) return false;
		std::lock_guard<std::mutex> passLock(
			AppInfoProvision::provisioningPassMutex());
		if (g_config.managedAppIds.get() != state.managedAppIds) return false;
		{
			std::lock_guard<std::mutex> publicationLock(
				AppInfoProvision::cachePublicationMutex());
			if (AppInfoProvision::cachePublicationGenerationLocked(baseAppId) !=
				expectedManagedGeneration) return false;
		}
		state.metadataRepairBaseIds.erase(
			std::remove(state.metadataRepairBaseIds.begin(),
				state.metadataRepairBaseIds.end(), baseAppId),
			state.metadataRepairBaseIds.end());
		state.metadataDeferredBaseIds.insert(baseAppId);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool publishMetadataCompletion(
	std::uint32_t baseAppId,
	std::uint64_t expectedManagedGeneration) noexcept
{
	try
	{
		if (baseAppId == 0) return false;
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> coordinatorLock(state.mutex);
		if (!state.initialized ||
			state.managedAppIds.count(baseAppId) == 0) return false;

		// Lock order matches publishLocked (coordinator -> pass -> publication).
		// Holding the pass boundary through submit prevents a config removal from
		// overtaking a just-validated completion generation.
		std::lock_guard<std::mutex> passLock(
			AppInfoProvision::provisioningPassMutex());
		const auto managed = g_config.managedAppIds.get();
		if (managed != state.managedAppIds ||
			managed.count(baseAppId) == 0) return false;
		{
			std::lock_guard<std::mutex> publicationLock(
				AppInfoProvision::cachePublicationMutex());
			if (AppInfoProvision::cachePublicationGenerationLocked(baseAppId) !=
				expectedManagedGeneration) return false;
		}

		const std::uint64_t nextGeneration = state.generation + 1;
		auto remainingPending = state.metadataPendingBaseIds;
		remainingPending.erase(baseAppId);
		// A migration request may have coalesced with a real hot-add while it was
		// queued. The live splice has succeeded at this point, so its child
		// metadata must participate in the completion generation.
		state.metadataDeferredBaseIds.erase(baseAppId);
		auto built = HotReloadInputs::buildFromCaches(
			nextGeneration, managed, remainingPending,
			state.metadataDeferredBaseIds, state.readyBaseIds);
		if (!built.valid)
		{
			return false;
		}
		if (state.hasLastSnapshot)
		{
			built.snapshot.appInfoRequestIds =
				HotReloadPublishPolicy::newTopologyAppInfoRequestIds(
					state.lastSnapshot, built.snapshot);
		}
		const bool snapshotChanged = !state.hasLastSnapshot ||
			HotReloadPublishPolicy::metadataSnapshotChanged(
				state.lastSnapshot, built.snapshot);
		if (!snapshotChanged)
		{
			// The metadata-only appinfo splice/reload already succeeded. If other
			// bases are still pending there is no package topology change to send,
			// but this base must still leave the repair set.
			state.metadataPendingBaseIds = std::move(remainingPending);
			state.metadataRepairBaseIds = built.metadataMissingBaseIds;
			state.cacheMtimeSecs = built.cacheMtimeSecs;
			state.cacheRepairBaseIds.erase(
				std::remove(state.cacheRepairBaseIds.begin(),
					state.cacheRepairBaseIds.end(), baseAppId),
				state.cacheRepairBaseIds.end());
			if (state.cacheRepairBaseIds.empty()) state.cacheRepairAfter = {};
			if (g_pLog != nullptr)
				g_pLog->info(
					"HotReload: DLC metadata live for base=%u; package topology unchanged\n",
					baseAppId);
			return true;
		}

		const auto authoritative =
			publishAppInfoScopes(managed, built.snapshot.appIds);
		built.snapshot.appInfoRequestIds =
			HotReloadPublishPolicy::nonAuthoritativeAppInfoRequestIds(
				std::move(built.snapshot.appInfoRequestIds), authoritative);
		const OwnerWork::Mode mode = OwnerWork::submitManagedState(built.snapshot);
		if (mode == OwnerWork::Mode::Abandoned) return false;

		state.generation = nextGeneration;
		state.metadataPendingBaseIds = std::move(remainingPending);
		state.metadataRepairBaseIds = built.metadataMissingBaseIds;
		state.cacheMtimeSecs = built.cacheMtimeSecs;
		state.cacheRepairBaseIds.erase(
			std::remove(state.cacheRepairBaseIds.begin(),
				state.cacheRepairBaseIds.end(), baseAppId),
			state.cacheRepairBaseIds.end());
		if (state.cacheRepairBaseIds.empty()) state.cacheRepairAfter = {};
		state.lastSnapshot = built.snapshot;
		state.hasLastSnapshot = true;
		if (g_pLog != nullptr)
		{
			g_pLog->info(
				"HotReload: generation %llu dispatched %s after DLC metadata completion "
				"(base=%u planner_apps=%zu depots=%zu)\n",
				static_cast<unsigned long long>(nextGeneration),
				OwnerWork::modeName(mode), baseAppId,
				built.snapshot.appIds.size(), built.snapshot.depotIds.size());
		}
		return true;
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"HotReload: DLC metadata completion failed; previous state retained\n");
		return false;
	}
}

void shutdown() noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.initialized)
			return;
		state.initialized = false;
		state.managedAppIds.clear();
		state.readyBaseIds.clear();
		state.contentFingerprints.clear();
		state.managedGenerations.clear();
		state.cacheMtimeSecs.clear();
		state.metadataPendingBaseIds.clear();
		state.metadataDeferredBaseIds.clear();
		state.cacheRepairBaseIds.clear();
		state.cacheRepairAfter = {};
		state.metadataRepairBaseIds.clear();
		state.metadataRepairAfter = {};
		state.metadataRepairOpportunitySeen = false;
		state.generation = 0;
		state.hasLastSnapshot = false;
		state.lastSnapshot = {};
	}
	catch (...)
	{
		// Teardown continues through Hooks::remove, which disables the appinfo
		// hook and closes the owner queue independently.
	}
}
} // namespace HotReload
