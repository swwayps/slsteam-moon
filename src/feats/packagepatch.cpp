// SPDX-License-Identifier: AGPL-3.0-only
//
// Package patch feature implementation.

#include "packagepatch.hpp"

#include "../afftrace.hpp"
#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "../sdk/CPackageInfo.hpp"
#include "../sdk/IClientApps.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"

#include "depotquarantine.hpp"
#include "depotquarantine_store.hpp"
#include "appinfostate.hpp"
#include "hotreload_package.hpp"
#include "hotreload_capabilities.hpp"
#include "license_refresh_policy.hpp"
#include "libraryremoval.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


namespace
{
	// LoadPackage(PackageInfo*, uint8* sha1, int32 cn, void* parserCtx) -> bool
	using LoadPackage_t = bool(*)(PackageInfo*, uint8_t*, int32_t, void*);

	// CUtlMemoryGrow takes the CUtlMemory base (i.e. &vec->m_Memory which
	// is &vec->m_Memory.m_pMemory because CUtlMemory's first member IS
	// m_pMemory) and a grow_size.  Returns void* (we don't use it).
	using CUtlMemoryGrow_t = void*(*)(void* /*pMem*/, int /*growSize*/);
	using MarkLicenseAsChanged_t = std::int64_t (__attribute__((cdecl)) *)(
		void*, std::uint32_t, bool);
	using ProcessPendingLicenseUpdates_t = bool (__attribute__((cdecl)) *)(void*);

	LoadPackage_t      g_pOrigLoadPackage = nullptr;
	CUtlMemoryGrow_t   g_pCUtlMemoryGrow  = nullptr;
	MarkLicenseAsChanged_t g_pMarkLicenseAsChanged = nullptr;
	ProcessPendingLicenseUpdates_t g_pProcessPendingLicenseUpdates = nullptr;

	lm_address_t g_loadPackageAddr = LM_ADDRESS_BAD;
	lm_address_t g_loadPackageTramp = LM_ADDRESS_BAD;
	lm_size_t    g_loadPackageHookSize = 0;

	// Saved pointer to package 0's PackageInfo — captured the first time
	// LoadPackage is called with PackageId == 0.  Used by
	// injectIntoPackage0 for re-injection if our config loaded late.
	std::atomic<PackageInfo*> g_pPackage0{nullptr};

	// Set of (appid, depotid) already appended to package 0, so we
	// don't double-insert and break ExistInPackageNums-based ownership
	// detection.
	std::mutex g_seededMutex;
	std::unordered_set<uint32_t> g_seededAppIds;
	std::unordered_set<uint32_t> g_seededDepotIds;

	// Guards the post-injection license-reconcile so we broadcast a
	// LicensesUpdated_t at most once per process.  Re-broadcasting on
	// every LoadPackage(0) call would spam the client UI; one reconcile
	// after the package-0 AppIdVec contains our apps is enough to break
	// the cold-cache PICS loop.
	std::atomic<bool> g_licenseReconciled{false};

	// Set true once we've actually appended managed app ids into
	// package 0.  The reconcile must not fire before this (there'd be
	// nothing to reconcile), and the CheckAppOwnership-driven retry
	// path keys off it.
	std::atomic<bool> g_package0Injected{false};

	// DLC appids (discovered from each AddedApp's provisioned appinfo)
	// that must ALSO be injected into package 0's AppIdVec on every
	// load, so the install planner schedules their `dlcappid` depots.
	// Guarded by g_seededMutex.  Merged with managed base app ids at
	// inject time.
	std::vector<uint32_t> g_extraAppIds;

	// Runtime desired state.  All fields are guarded by g_seededMutex.  The
	// snapshot is stored even while package 0 is unavailable so a later
	// LoadPackage(0) can reconcile the exact latest state.
	PackageSnapshot g_desiredSnapshot;
	bool g_hasDesiredSnapshot = false;
	std::uint64_t g_appliedGeneration = 0;
	std::uint64_t g_processedGeneration = 0;
	std::uint64_t g_pendingGeneration = 0;
	std::uint64_t g_deferredLogGeneration = 0;
	bool g_deferredLogValid = false;
	bool g_runtimeRefreshPending = false;
	std::atomic<bool> g_runtimeRefreshPendingHint{false};
	std::unordered_set<std::uint32_t> g_pendingMetadataAppIds;
	HotReloadPackage::AppInfoRequestState g_appInfoRequestState;
	std::uint64_t g_fallbackReconcileGeneration = 0;
	thread_local bool t_processingRuntimeRefresh = false;
	std::atomic<bool> g_runtimeRefreshExecuting{false};

	// Force Steam to re-read licenses (and therefore package 0, now
	// holding our injected managed ids) by broadcasting a
	// LicensesUpdated_t on the local CUser.  This is the missing
	// "license reconcile" step: without it, injecting appids into
	// package 0's AppIdVec on a COLD cache leaves Steam re-requesting
	// PICS product-info for those apps forever ("Loading user data"
	// hang).  Safe no-op when nothing has been injected yet, when the
	// local user isn't available yet (retried later), or when the
	// NotifyLicensesUpdated pattern didn't resolve.  Broadcasts once.
	void reconcileLicensesOnce()
	{
		if (g_licenseReconciled.load(std::memory_order_acquire))
		{
			return;
		}
		if (!g_package0Injected.load(std::memory_order_acquire))
		{
			// Nothing injected yet — nothing to reconcile.
			return;
		}

		CUser* user = getLocalUser();
		if (user == nullptr)
		{
			// No usable CUser yet (very early on a cold cache, before
			// the engine user map is populated and before
			// CheckAppOwnership has captured one).  Leave the flag
			// unset; the CheckAppOwnership-driven retry will call us
			// again once a user exists.
			g_pLog->debugOnce("PackagePatch: license reconcile deferred (no local user yet)\n");
			return;
		}

		bool notified = false;
		{
			// Exact Steam-owned call site (see afftrace.hpp).
			auto span = AffTrace::fnSpan(AffTrace::Call::NotifyLicensesUpdated,
			                             AffTrace::Mode::NA);
			notified = user->notifyLicensesUpdated();
		}
		if (notified)
		{
			g_licenseReconciled.store(true, std::memory_order_release);
			g_pLog->info("PackagePatch: broadcast LicensesUpdated_t to reconcile package 0\n");
		}
		else
		{
			g_pLog->warn
			(
				"PackagePatch: NotifyLicensesUpdated unavailable; "
				"cold-cache reconcile skipped (warm cache still works)\n"
			);
			// Mark as done so we don't log this every call.
			g_licenseReconciled.store(true, std::memory_order_release);
		}
	}

	void reconcileGenerationOnce(std::uint64_t generation)
	{
		bool claimed = false;
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			if (generation > g_fallbackReconcileGeneration)
			{
				g_fallbackReconcileGeneration = generation;
				claimed = true;
			}
		}
		if (!claimed) return;
		g_licenseReconciled.store(false, std::memory_order_release);
		reconcileLicensesOnce();
	}

	// Walk the SLSsteam depot-key cache (`<config>/cache/depotkey_*.yaml`)
	// and return every depot id whose recorded appId is in `appFilter`.
	// Each cache file is plain YAML with `appId:` and `depotId:` fields.
	std::vector<uint32_t> collectDepotsForApps(const std::unordered_set<uint32_t>& appFilter)
	{
		std::vector<uint32_t> out;
		const auto dir = std::filesystem::path(g_config.getDir().c_str()) / "cache";
		if (appFilter.empty() || !std::filesystem::is_directory(dir))
		{
			return out;
		}

		static const std::regex appIdRe("^appId:\\s*(\\d+)\\s*$");
		static const std::regex depotIdRe("^depotId:\\s*(\\d+)\\s*$");

		for (const auto& entry : std::filesystem::directory_iterator(dir))
		{
			if (!entry.is_regular_file()) continue;
			const auto& p = entry.path();
			const auto name = p.filename().string();
			if (name.rfind("depotkey_", 0) != 0 || p.extension() != ".yaml")
			{
				continue;
			}

			std::ifstream f(p);
			if (!f.is_open()) continue;

			uint32_t fileAppId = 0;
			uint32_t fileDepotId = 0;
			std::string line;
			while (std::getline(f, line))
			{
				std::smatch m;
				if (std::regex_match(line, m, appIdRe))
				{
					fileAppId = static_cast<uint32_t>(std::stoul(m[1]));
				}
				else if (std::regex_match(line, m, depotIdRe))
				{
					fileDepotId = static_cast<uint32_t>(std::stoul(m[1]));
				}
			}

			if (fileAppId && fileDepotId && appFilter.count(fileAppId))
			{
				out.push_back(fileDepotId);
			}
		}
		return out;
	}

	struct VectorPlan
	{
		std::unordered_set<std::uint32_t> desired;
		std::set<std::uint32_t> missing;
		std::uint32_t targetSize = 0;
		bool removes = false;
	};

	struct SnapshotApplyResult
	{
		bool available = false;
		bool changed = false;
		bool appAdded = false;
		std::vector<std::uint32_t> appInfoRequestIds;
	};

	enum class RuntimeRefreshResult
	{
		Complete,
		Deferred,
		Busy,
	};

	bool validLiveVector(const CUtlVector<std::uint32_t>& vec) noexcept
	{
		return vec.m_Size <= vec.m_Memory.m_nAllocationCount &&
			(vec.m_Size == 0 || vec.m_Memory.m_pMemory != nullptr);
	}

	bool buildVectorPlan(
		const CUtlVector<std::uint32_t>& vec,
		const std::unordered_set<std::uint32_t>& seeded,
		const std::vector<std::uint32_t>& desiredValues,
		VectorPlan& plan
	)
	{
		if (!validLiveVector(vec))
			return false;

		plan = {};
		for (const std::uint32_t id : desiredValues)
		{
			if (id != 0)
				plan.desired.insert(id);
		}

		std::uint32_t kept = 0;
		for (std::uint32_t index = 0; index < vec.m_Size; ++index)
		{
			const std::uint32_t id = vec.m_Memory.m_pMemory[index];
			if (seeded.count(id) != 0 && plan.desired.count(id) == 0)
			{
				plan.removes = true;
				continue;
			}
			++kept;
		}

		plan.missing = HotReloadPackage::missingFromVector(
			vec.m_Memory.m_pMemory, vec.m_Size, plan.desired);
		const std::size_t target = static_cast<std::size_t>(kept) +
			plan.missing.size();
		if (target > std::numeric_limits<std::uint32_t>::max())
			return false;
		plan.targetSize = static_cast<std::uint32_t>(target);
		return true;
	}

	bool prepareVectorLocked(
		CUtlVector<std::uint32_t>& vec,
		const VectorPlan& plan,
		const char* label
	)
	{
		if (!validLiveVector(vec))
			return false;
		if (vec.m_Memory.m_nAllocationCount >= plan.targetSize)
			return true;
		if (g_pCUtlMemoryGrow == nullptr || plan.targetSize <= vec.m_Size)
			return false;

		const std::uint32_t growBy = plan.targetSize - vec.m_Size;
		if (growBy > static_cast<std::uint32_t>(
				std::numeric_limits<int>::max()))
		{
			return false;
		}
		{
			auto span = AffTrace::fnSpan(
				AffTrace::Call::CutlMemoryGrow, AffTrace::Mode::NA);
			g_pCUtlMemoryGrow(&vec.m_Memory, static_cast<int>(growBy));
		}
		if (!validLiveVector(vec) ||
			vec.m_Memory.m_nAllocationCount < plan.targetSize)
		{
			g_pLog->warn(
				"PackagePatch: %s capacity preparation failed (%u required, %u available)\n",
				label, plan.targetSize, vec.m_Memory.m_nAllocationCount);
			return false;
		}
		return true;
	}

	std::unordered_set<std::uint32_t> buildNextSeeded(
		const std::unordered_set<std::uint32_t>& seeded,
		const VectorPlan& plan)
	{
		std::unordered_set<std::uint32_t> next;
		next.reserve(seeded.size() + plan.missing.size());
		for (const std::uint32_t id : seeded)
		{
			if (plan.desired.count(id) != 0)
				next.insert(id);
		}
		next.insert(plan.missing.begin(), plan.missing.end());
		return next;
	}

	bool applyVectorPlanLocked(
		CUtlVector<std::uint32_t>& vec,
		const std::unordered_set<std::uint32_t>& seeded,
		const VectorPlan& plan
	) noexcept
	{
		const std::uint32_t previousSize = vec.m_Size;
		HotReloadPackage::compactInjected(
			vec.m_Memory.m_pMemory, vec.m_Size, seeded, plan.desired);

		for (const std::uint32_t id : plan.missing)
			vec.m_Memory.m_pMemory[vec.m_Size++] = id;

		return plan.removes || !plan.missing.empty() || vec.m_Size != previousSize;
	}

	SnapshotApplyResult applySnapshotLocked(
		PackageInfo* package,
		const PackageSnapshot& snapshot
	)
	{
		SnapshotApplyResult result;
		if (package == nullptr || package->PackageId != 0 ||
			package->Status != EPackageStatus::Available)
		{
			return result;
		}

		const auto excluded = DepotQuarantine::package0Exclusions();
		const auto desiredApps = DepotQuarantineStore::withoutIds(
			snapshot.appIds, excluded);
		const auto desiredDepots = DepotQuarantineStore::withoutIds(
			snapshot.depotIds, excluded);
		VectorPlan appPlan;
		VectorPlan depotPlan;
		if (!buildVectorPlan(package->AppIdVec, g_seededAppIds,
				desiredApps, appPlan) ||
			!buildVectorPlan(package->DepotIdVec, g_seededDepotIds,
				desiredDepots, depotPlan))
		{
			g_pLog->warn(
				"PackagePatch: package 0 vectors are invalid; runtime sync deferred\n");
			return result;
		}
		auto nextSeededApps = buildNextSeeded(g_seededAppIds, appPlan);
		auto nextSeededDepots = buildNextSeeded(g_seededDepotIds, depotPlan);
		auto appInfoRequestIds =
			HotReloadPackage::snapshotAppInfoRequestIdsAfterApply(true, snapshot);
		auto nextPendingMetadata = g_pendingMetadataAppIds;
		if (snapshot.metadataComplete)
		{
			nextPendingMetadata.clear();
		}
		else
		{
			for (auto current = nextPendingMetadata.begin();
				current != nextPendingMetadata.end();)
			{
				if (appPlan.desired.count(*current) == 0)
					current = nextPendingMetadata.erase(current);
				else
					++current;
			}
			nextPendingMetadata.insert(
				snapshot.addedAppIds.begin(), snapshot.addedAppIds.end());
		}

		// Capacity for both vectors is prepared before either logical size or
		// element is changed.  A failed grow can reallocate capacity but cannot
		// publish a half-reconciled logical package.
		if (!prepareVectorLocked(package->AppIdVec, appPlan, "AppIdVec") ||
			!prepareVectorLocked(package->DepotIdVec, depotPlan, "DepotIdVec"))
		{
			return result;
		}

		const bool appsChanged = applyVectorPlanLocked(
			package->AppIdVec, g_seededAppIds, appPlan);
		const bool depotsChanged = applyVectorPlanLocked(
			package->DepotIdVec, g_seededDepotIds, depotPlan);
		if (!validLiveVector(package->AppIdVec) ||
			!validLiveVector(package->DepotIdVec))
		{
			// All remaining operations are fixed-size writes, so this indicates
			// external corruption.  Do not publish the generation or refresh.
			g_pLog->warn(
				"PackagePatch: package 0 vectors became invalid during runtime sync\n");
			return result;
		}
		g_seededAppIds.swap(nextSeededApps);
		g_seededDepotIds.swap(nextSeededDepots);
		g_pendingMetadataAppIds.swap(nextPendingMetadata);

		result.available = true;
		result.changed = appsChanged || depotsChanged;
		result.appInfoRequestIds = std::move(appInfoRequestIds);
		result.appAdded = !snapshot.addedAppIds.empty();
		g_appliedGeneration = snapshot.generation;
		if (result.appAdded)
			g_package0Injected.store(true, std::memory_order_release);
		return result;
	}

	void logDeferredOnce(std::uint64_t generation, const char* reason)
	{
		bool shouldLog = false;
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			if (!g_deferredLogValid || g_deferredLogGeneration != generation)
			{
				g_deferredLogGeneration = generation;
				g_deferredLogValid = true;
				shouldLog = true;
			}
		}
		if (shouldLog)
		{
			g_pLog->warn(
				"PackagePatch: runtime refresh generation %llu deferred (%s); restart remains available\n",
				static_cast<unsigned long long>(generation), reason);
		}
	}

	void requestLiveAppInfo(
		std::uint64_t generation,
		const std::vector<std::uint32_t>& appIds)
	{
		if (appIds.empty()) return;
		std::vector<std::uint32_t> pending;
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			pending = g_appInfoRequestState.reserve(generation, appIds);
		}
		if (pending.empty()) return;

		bool accepted = false;
		if (g_pClientApps == nullptr)
		{
			g_pLog->info(
				"PackagePatch: live appinfo request generation %llu deferred "
				"(client apps unavailable)\n",
				static_cast<unsigned long long>(generation));
		}
		else if (g_pClientApps->requestAppInfoUpdate(pending))
		{
			accepted = true;
			g_pLog->info(
				"PackagePatch: requested live appinfo for %zu app(s) "
				"in generation %llu\n",
				pending.size(), static_cast<unsigned long long>(generation));
		}
		else
		{
			g_pLog->info(
				"PackagePatch: live appinfo request generation %llu not accepted "
				"(client offline or updater unavailable)\n",
				static_cast<unsigned long long>(generation));
		}
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			g_appInfoRequestState.finish(generation, pending, accepted);
		}
	}

	RuntimeRefreshResult processRuntimeRefresh(
		std::uint64_t generation,
		bool unresolvedStateSafe,
		bool changed
	)
	{
		using LicenseRefreshPolicy::Action;
		const Action action = LicenseRefreshPolicy::decide(
			g_pMarkLicenseAsChanged != nullptr,
			g_pProcessPendingLicenseUpdates != nullptr,
			unresolvedStateSafe,
			changed);
		if (action == Action::NoChange)
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			g_processedGeneration = std::max(g_processedGeneration, generation);
			if (g_runtimeRefreshPending && g_pendingGeneration == generation)
			{
				g_runtimeRefreshPending = false;
				g_pendingGeneration = 0;
			}
			g_runtimeRefreshPendingHint.store(
				g_runtimeRefreshPending, std::memory_order_release);
			return RuntimeRefreshResult::Complete;
		}
		if (action == Action::Defer)
		{
			const char* reason = !unresolvedStateSafe
				? "app metadata is unresolved and the guard is unavailable"
				: "mark/process capability is unavailable";
			logDeferredOnce(generation, reason);
			return RuntimeRefreshResult::Deferred;
		}
		if (t_processingRuntimeRefresh)
			return RuntimeRefreshResult::Busy;
		bool expected = false;
		if (!g_runtimeRefreshExecuting.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
			return RuntimeRefreshResult::Busy;
		t_processingRuntimeRefresh = true;
		struct ProcessingGuard
		{
			~ProcessingGuard()
			{
				t_processingRuntimeRefresh = false;
				g_runtimeRefreshExecuting.store(false, std::memory_order_release);
			}
		} processingGuard;

		CUser* const user = getLocalUser();
		if (user == nullptr)
		{
			logDeferredOnce(generation, "the local user is not ready");
			return RuntimeRefreshResult::Deferred;
		}

		{
			auto span = AffTrace::fnSpan(
				AffTrace::Call::MarkLicenseChanged, AffTrace::Mode::NA);
			(void)g_pMarkLicenseAsChanged(user, 0, true);
		}
		bool processed = false;
		{
			auto span = AffTrace::fnSpan(
				AffTrace::Call::ProcessLicenseUpdates, AffTrace::Mode::NA);
			processed = g_pProcessPendingLicenseUpdates(user);
		}
		if (!processed)
		{
			logDeferredOnce(generation, "Steam retained the pending update");
			return RuntimeRefreshResult::Deferred;
		}

		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			g_processedGeneration = std::max(g_processedGeneration, generation);
			if (g_runtimeRefreshPending && g_pendingGeneration == generation)
			{
				g_runtimeRefreshPending = false;
				g_pendingGeneration = 0;
			}
			g_runtimeRefreshPendingHint.store(
				g_runtimeRefreshPending, std::memory_order_release);
		}
		g_pLog->info(
			"PackagePatch: processed runtime package change generation %llu\n",
			static_cast<unsigned long long>(generation));
		return RuntimeRefreshResult::Complete;
	}

	// Generic append-to-CUtlVector helper.  Caller holds g_seededMutex.
	// `seenSet` holds previously-injected ids to avoid duplicates.
	uint32_t appendToVecLocked(CUtlVector<uint32_t>& vec,
	                           const std::vector<uint32_t>& ids,
	                           std::unordered_set<uint32_t>& seenSet,
	                           const char* vecLabel)
	{
		if (!g_pCUtlMemoryGrow || ids.empty())
		{
			return 0;
		}

		std::vector<uint32_t> fresh;
		fresh.reserve(ids.size());
		for (uint32_t id : ids)
		{
			if (id && !seenSet.count(id))
			{
				fresh.push_back(id);
			}
		}
		if (fresh.empty())
		{
			return 0;
		}

		const uint32_t oldSize = vec.m_Size;
		const uint32_t toAdd = static_cast<uint32_t>(fresh.size());
		{
			// Exact Steam-owned call site (see afftrace.hpp): resolved
			// CUtlMemoryGrow against a live Steam vector.
			auto span = AffTrace::fnSpan(AffTrace::Call::CutlMemoryGrow,
			                             AffTrace::Mode::NA);
			g_pCUtlMemoryGrow(&vec.m_Memory, static_cast<int>(toAdd));
		}

		const uint32_t available = vec.m_Memory.m_nAllocationCount;
		if (available < oldSize + toAdd)
		{
			g_pLog->warn
			(
				"PackagePatch: %s grow returned only %u slots for %u + %u; "
				"aborting injection\n",
				vecLabel, available, oldSize, toAdd
			);
			return 0;
		}
		if (!vec.m_Memory.m_pMemory)
		{
			g_pLog->warn("PackagePatch: %s.m_pMemory still null after Grow\n", vecLabel);
			return 0;
		}

		for (uint32_t i = 0; i < toAdd; ++i)
		{
			vec.m_Memory.m_pMemory[oldSize + i] = fresh[i];
			seenSet.insert(fresh[i]);
		}
		vec.m_Size = oldSize + toAdd;
		return toAdd;
	}

	// Append `appIds` (and their associated depots) to pkg 0.  Caller
	// holds g_seededMutex.
	uint32_t injectFullSetLocked(PackageInfo* pPkg, const std::vector<uint32_t>& appIds)
	{
		if (!pPkg || !g_pCUtlMemoryGrow || appIds.empty())
		{
			return 0;
		}

		// A DLC whose decryption key is proven unusable must not be injected:
		// Steam rebuilds the app's DESIRED configuration from package 0, so
		// keeping it here makes Steam re-add the depot after every successful
		// commit ("config changed : added depots <dlc>"), re-plan, and retry
		// chunks it can never decrypt — which also marks every CDN source bad
		// for the whole client and breaks unrelated downloads.  Excluding it
		// here costs that one DLC and leaves normal updates enabled.
		const auto excluded = DepotQuarantine::package0Exclusions();
		const auto injectable = DepotQuarantineStore::withoutIds(appIds, excluded);
		if (injectable.empty())
		{
			return 0;
		}

		const uint32_t appsAdded = appendToVecLocked(
			pPkg->AppIdVec, injectable, g_seededAppIds, "AppIdVec");

		// Resolve depots from the SLSsteam depot-key cache.  Steam's depot
		// eligibility filter looks at pkg.DepotIdVec — if we only added
		// the appid, the install dialog still reports 0 B because no
		// depots are eligible.
		std::unordered_set<uint32_t> appFilter(injectable.begin(), injectable.end());
		const auto depotIds = DepotQuarantineStore::withoutIds(
			collectDepotsForApps(appFilter), excluded);
		const uint32_t depotsAdded = appendToVecLocked(
			pPkg->DepotIdVec, depotIds, g_seededDepotIds, "DepotIdVec");

		if (appsAdded || depotsAdded)
		{
			g_pLog->info
			(
				"PackagePatch: package 0 now has AppIdVec.m_Size=%u "
				"DepotIdVec.m_Size=%u (added %u apps, %u depots)\n",
				pPkg->AppIdVec.m_Size,
				pPkg->DepotIdVec.m_Size,
				appsAdded,
				depotsAdded
			);
		}
		if (appsAdded)
		{
			g_package0Injected.store(true, std::memory_order_release);
		}
		return appsAdded;
	}

	// LoadPackage detour.  Lets Steam run first (so `pInfo` is fully
	// populated), then if the package is 0 and Status is Available we
	// append the managed desired app ids to AppIdVec.
	bool hkLoadPackage(PackageInfo* pInfo, uint8_t* sha1, int32_t cn, void* p4)
	{
		const bool result = g_pOrigLoadPackage(pInfo, sha1, cn, p4);

		if (!pInfo)
		{
			return result;
		}

		g_pLog->debug
		(
			"LoadPackage: PackageId=%u Status=%d AppIdVec.m_Size=%u result=%d\n",
			pInfo->PackageId,
			static_cast<int>(pInfo->Status),
			pInfo->AppIdVec.m_Size,
			result
		);

		if (pInfo->PackageId != 0)
		{
			return result;
		}

		// Skip injection if Steam reports the package as not
		// Available.  Injecting into a stale/Invalid package gets
		// the vector clobbered when Steam re-loads it, costing us
		// every appid we added.  Stash the pointer so a later
		// re-inject (after Steam settles) can pick up.
		g_pPackage0.store(pInfo, std::memory_order_release);

		if (pInfo->Status != EPackageStatus::Available)
		{
			g_pLog->debug
			(
				"LoadPackage(PackageId=0): status=%d not Available; "
				"deferring injection\n",
				static_cast<int>(pInfo->Status)
			);
			return result;
		}

		// Once the runtime coordinator has published a complete snapshot it is
		// authoritative across every later package reload.  Before that point,
		// retain the boot path, but source it only from the managed stplug-in /
		// luaappids union plus its discovered planner IDs.
		try
		{
			SnapshotApplyResult applied;
			PackageSnapshot desired;
			bool hasDesired = false;
			bool refreshNeeded = false;
			bool unresolvedStateSafe = false;
			bool allowColdFallbackReconcile = false;
			{
				std::lock_guard<std::mutex> lock(g_seededMutex);
				if (g_hasDesiredSnapshot)
				{
					allowColdFallbackReconcile = g_processedGeneration == 0;
					desired = g_desiredSnapshot;
					applied = applySnapshotLocked(pInfo, desired);
					if (applied.available)
					{
						if (applied.changed)
						{
							g_runtimeRefreshPending = true;
							g_runtimeRefreshPendingHint.store(
								true, std::memory_order_release);
							g_pendingGeneration = desired.generation;
						}
						else if (g_runtimeRefreshPending)
						{
							g_pendingGeneration = desired.generation;
						}
						refreshNeeded = g_runtimeRefreshPending;
						unresolvedStateSafe =
							LicenseRefreshPolicy::unresolvedStateSafe(
								!g_pendingMetadataAppIds.empty(),
								AppInfoState::ready());
					}
					hasDesired = true;
				}
			}
			if (hasDesired)
			{
				if (!applied.available)
				{
					{
						std::lock_guard<std::mutex> lock(g_seededMutex);
						g_pendingGeneration = desired.generation;
					}
					logDeferredOnce(
						desired.generation, "package 0 apply did not complete");
					return result;
				}
				if (applied.changed)
				{
					g_pLog->info(
						"PackagePatch: reapplied runtime package state generation %llu\n",
						static_cast<unsigned long long>(desired.generation));
				}
				// A snapshot can arrive before Steam exposes package 0. Complete its
				// generation-scoped license/UI work after the deferred apply.
				RuntimeRefreshResult refreshResult = RuntimeRefreshResult::Busy;
				if (!t_processingRuntimeRefresh)
				{
					refreshResult = processRuntimeRefresh(
						desired.generation, unresolvedStateSafe, refreshNeeded);
				}
				if (refreshResult == RuntimeRefreshResult::Complete)
				{
					for (const std::uint32_t appId : desired.addedAppIds)
						LibraryRemoval::restore(appId);
				}
				else if (refreshResult == RuntimeRefreshResult::Deferred &&
					allowColdFallbackReconcile &&
					!t_processingRuntimeRefresh)
				{
					reconcileGenerationOnce(desired.generation);
				}
				if (refreshResult != RuntimeRefreshResult::Busy &&
					!t_processingRuntimeRefresh)
				{
					requestLiveAppInfo(
						desired.generation, applied.appInfoRequestIds);
				}
				return result;
			}
		}
		catch (...)
		{
			g_pLog->warn(
				"PackagePatch: runtime package reapply failed; current Steam state retained\n");
			return result;
		}

		const auto managed = g_config.managedAppIds.get();
		std::vector<uint32_t> ids(managed.begin(), managed.end());
		{
			// Append the registered DLC appids (under the same lock that
			// guards g_extraAppIds and the seeding sets below).
			std::lock_guard<std::mutex> lk(g_seededMutex);
			ids.insert(ids.end(), g_extraAppIds.begin(), g_extraAppIds.end());
		}
		if (ids.empty())
		{
			return result;
		}

		{
			std::lock_guard<std::mutex> lk(g_seededMutex);
			(void)injectFullSetLocked(pInfo, ids);
		}

		// After the package-0 AppIdVec actually contains our apps,
		// broadcast a license update so Steam re-reads ownership.
		// Without this the cold cache hangs at "Loading user data".
		// Also run it even when nothing was added on the first pass: a
		// warm-ish cache may have seeded earlier, but the reconcile is
		// gated to fire once regardless, and is a safe no-op if a user
		// isn't ready yet (it retries on the next LoadPackage(0)).
		reconcileLicensesOnce();
		return result;
	}
}

namespace PackagePatch
{
	bool setup()
	{
		// Both patterns are required.  If either fails to resolve we
		// disable the feature rather than installing a half-working hook.
		if (Patterns::CPackageInfoCache::LoadPackage.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("PackagePatch: LoadPackage pattern not found; feature disabled\n");
			return false;
		}
		if (Patterns::CUtlMemory::Grow.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("PackagePatch: CUtlMemoryGrow pattern not found; feature disabled\n");
			return false;
		}

		g_pCUtlMemoryGrow = reinterpret_cast<CUtlMemoryGrow_t>(
			Patterns::CUtlMemory::Grow.address);
		g_pMarkLicenseAsChanged =
			Patterns::CUser::MarkLicenseAsChanged.address == LM_ADDRESS_BAD
				? nullptr
				: reinterpret_cast<MarkLicenseAsChanged_t>(
					Patterns::CUser::MarkLicenseAsChanged.address);
		g_pProcessPendingLicenseUpdates =
			Patterns::CUser::ProcessPendingLicenseUpdates.address == LM_ADDRESS_BAD
				? nullptr
				: reinterpret_cast<ProcessPendingLicenseUpdates_t>(
					Patterns::CUser::ProcessPendingLicenseUpdates.address);
		if (g_pMarkLicenseAsChanged == nullptr ||
			g_pProcessPendingLicenseUpdates == nullptr)
		{
			g_pLog->warn(
				"PackagePatch: runtime mark/process capability incomplete; boot path remains available\n");
		}

		g_loadPackageAddr  = Patterns::CPackageInfoCache::LoadPackage.address;
		g_loadPackageHookSize = LM_HookCode
		(
			g_loadPackageAddr,
			reinterpret_cast<lm_address_t>(&hkLoadPackage),
			&g_loadPackageTramp
		);
		if (!g_loadPackageHookSize || g_loadPackageTramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("PackagePatch: failed to install LoadPackage hook\n");
			g_loadPackageHookSize = 0;
			return false;
		}

		MemHlp::fixPICThunkCall("CPackageInfoCache::LoadPackage",
			g_loadPackageAddr, g_loadPackageTramp);

		g_pOrigLoadPackage = reinterpret_cast<LoadPackage_t>(g_loadPackageTramp);

		g_pLog->debug
		(
			"PackagePatch: LoadPackage detour at %p, tramp at %p, CUtlMemoryGrow at %p\n",
			reinterpret_cast<void*>(g_loadPackageAddr),
			reinterpret_cast<void*>(g_loadPackageTramp),
			reinterpret_cast<void*>(g_pCUtlMemoryGrow)
		);
		return true;
	}

	void remove()
	{
		if (g_loadPackageHookSize && g_loadPackageAddr != LM_ADDRESS_BAD
		    && g_loadPackageTramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_loadPackageAddr, g_loadPackageTramp, g_loadPackageHookSize);
			g_loadPackageHookSize = 0;
		}
		g_pOrigLoadPackage = nullptr;
		g_pCUtlMemoryGrow  = nullptr;
		g_pMarkLicenseAsChanged = nullptr;
		g_pProcessPendingLicenseUpdates = nullptr;
		g_pPackage0.store(nullptr, std::memory_order_release);
		g_licenseReconciled.store(false, std::memory_order_release);
		g_package0Injected.store(false, std::memory_order_release);
		std::lock_guard<std::mutex> lk(g_seededMutex);
		g_seededAppIds.clear();
		g_seededDepotIds.clear();
		g_extraAppIds.clear();
		g_desiredSnapshot = {};
		g_hasDesiredSnapshot = false;
		g_appliedGeneration = 0;
		g_processedGeneration = 0;
		g_pendingGeneration = 0;
		g_deferredLogGeneration = 0;
		g_deferredLogValid = false;
		g_runtimeRefreshPending = false;
		g_runtimeRefreshPendingHint.store(false, std::memory_order_release);
		g_runtimeRefreshExecuting.store(false, std::memory_order_release);
		g_pendingMetadataAppIds.clear();
		g_appInfoRequestState.clear();
		g_fallbackReconcileGeneration = 0;
	}

	void setExtraAppIds(const std::vector<uint32_t>& appIds)
	{
		std::lock_guard<std::mutex> lk(g_seededMutex);
		g_extraAppIds = appIds;
	}

	bool injectIntoPackage0(const std::vector<uint32_t>& appIds)
	{
		PackageInfo* pPkg = g_pPackage0.load(std::memory_order_acquire);
		if (!pPkg || !g_pCUtlMemoryGrow || appIds.empty())
		{
			return false;
		}
		bool injected;
		{
			std::lock_guard<std::mutex> lk(g_seededMutex);
			injected = injectFullSetLocked(pPkg, appIds) > 0;
		}

		// Reconcile after the manual re-inject path too (covers the
		// case where Steam loaded package 0 before our hook was placed).
		reconcileLicensesOnce();
		return injected;
	}

	void tryReconcileLicenses()
	{
		// Cheap, lock-free retry entry point driven by a hook that
		// reliably has a valid local user (CheckAppOwnership).  No-op
		// after the one-shot broadcast has happened, or before package
		// 0 has actually been injected.
		reconcileLicensesOnce();
	}

	void forceReconcileLicenses()
	{
		// Hot-reload: re-arm the one-shot gate and broadcast again so the
		// reconcile can fire after boot.  Still guarded on g_package0Injected
		// and a valid local user, so it's a safe no-op early on.
		if (!g_package0Injected.load(std::memory_order_acquire))
		{
			return;
		}
		g_licenseReconciled.store(false, std::memory_order_release);
		reconcileLicensesOnce();
	}

	void synchronizePackage0(const PackageSnapshot& snapshot)
	{
		SnapshotApplyResult applied;
		PackageSnapshot desired = snapshot;
		bool refreshNeeded = false;
		bool unresolvedStateSafe = false;
		bool stale = false;
		bool conflicting = false;
		{
			std::lock_guard<std::mutex> lock(g_seededMutex);
			if (g_hasDesiredSnapshot &&
				snapshot.generation < g_desiredSnapshot.generation)
			{
				stale = true;
			}
			else if (g_hasDesiredSnapshot &&
				snapshot.generation == g_desiredSnapshot.generation &&
				!(snapshot == g_desiredSnapshot))
			{
				// OwnerQueue already rejects this case.  Keep the first complete
				// value if a direct caller violates that invariant.
				conflicting = true;
			}
			else
			{
				if (g_hasDesiredSnapshot)
				{
					const bool previousAppInfoRequested =
						g_appInfoRequestState.allAccepted(
							g_desiredSnapshot.generation,
							g_desiredSnapshot.appInfoRequestIds);
					desired = HotReloadPackage::carryPendingSnapshotWork(
						g_desiredSnapshot, desired,
						g_processedGeneration >= g_desiredSnapshot.generation,
						previousAppInfoRequested);
				}
				g_desiredSnapshot = desired;
				g_hasDesiredSnapshot = true;
				applied = applySnapshotLocked(
					g_pPackage0.load(std::memory_order_acquire), desired);
				if (applied.available)
				{
					if (applied.changed)
					{
						g_runtimeRefreshPending = true;
						g_runtimeRefreshPendingHint.store(
							true, std::memory_order_release);
						g_pendingGeneration = snapshot.generation;
					}
					else if (g_runtimeRefreshPending)
					{
						// The desired value can advance while an earlier mutation is
						// still waiting for mark/process capability.
						g_pendingGeneration = snapshot.generation;
					}
					refreshNeeded = g_runtimeRefreshPending;
					unresolvedStateSafe =
						LicenseRefreshPolicy::unresolvedStateSafe(
							!g_pendingMetadataAppIds.empty(), AppInfoState::ready());
				}
			}
		}

		if (stale)
		{
			g_pLog->debug(
				"PackagePatch: ignored stale runtime package generation %llu\n",
				static_cast<unsigned long long>(snapshot.generation));
			return;
		}
		if (conflicting)
		{
			g_pLog->warn(
				"PackagePatch: ignored conflicting payload for runtime package generation %llu\n",
				static_cast<unsigned long long>(snapshot.generation));
			return;
		}
		if (!applied.available)
		{
			logDeferredOnce(snapshot.generation, "package 0 is not ready");
			return;
		}

		if (applied.changed)
		{
			g_pLog->info(
				"PackagePatch: synchronized runtime package generation %llu\n",
				static_cast<unsigned long long>(snapshot.generation));
		}
		const RuntimeRefreshResult refreshResult = processRuntimeRefresh(
			snapshot.generation, unresolvedStateSafe, refreshNeeded);
		if (refreshResult == RuntimeRefreshResult::Complete)
		{
			for (const std::uint32_t appId : desired.addedAppIds)
				LibraryRemoval::restore(appId);
		}

		if (refreshResult != RuntimeRefreshResult::Busy)
			requestLiveAppInfo(snapshot.generation, applied.appInfoRequestIds);
	}

	bool runtimeRefreshReady()
	{
		HotReloadCapabilities::Matrix capabilities;
		capabilities.markLicenseChanged =
			g_pMarkLicenseAsChanged != nullptr;
		capabilities.processLicenseUpdates =
			g_pProcessPendingLicenseUpdates != nullptr;
		capabilities.unresolvedAppGuard = AppInfoState::ready();
		return capabilities.canProcessUnresolvedAdd();
	}

	bool runtimeRefreshPending() noexcept
	{
		return g_runtimeRefreshPendingHint.load(std::memory_order_acquire);
	}

	void reprocessCurrentState() noexcept
	{
		try
		{
			std::uint64_t generation = 0;
			std::vector<std::uint32_t> addedAppIds;
			std::vector<std::uint32_t> appInfoRequestIds;
			bool unresolvedStateSafe = false;
			{
				std::lock_guard<std::mutex> lock(g_seededMutex);
				if (!g_runtimeRefreshPending || !g_hasDesiredSnapshot ||
					g_appliedGeneration != g_desiredSnapshot.generation)
				{
					return;
				}
				generation = g_appliedGeneration;
				addedAppIds = g_desiredSnapshot.addedAppIds;
				appInfoRequestIds = g_desiredSnapshot.appInfoRequestIds;
				g_pendingGeneration = generation;
				unresolvedStateSafe =
					LicenseRefreshPolicy::unresolvedStateSafe(
						!g_pendingMetadataAppIds.empty(), AppInfoState::ready());
			}
			const RuntimeRefreshResult result = processRuntimeRefresh(
				generation, unresolvedStateSafe, true);
			if (result == RuntimeRefreshResult::Complete)
			{
				for (const std::uint32_t appId : addedAppIds)
					LibraryRemoval::restore(appId);
			}
			if (result != RuntimeRefreshResult::Busy)
				requestLiveAppInfo(generation, appInfoRequestIds);
		}
		catch (...)
		{
			// Optional runtime refresh must never unwind through Steam's frame.
			return;
		}
	}
}
