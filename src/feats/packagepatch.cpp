// SPDX-License-Identifier: AGPL-3.0-only
//
// Package patch feature implementation.

#include "packagepatch.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "../sdk/CPackageInfo.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>


namespace
{
	// LoadPackage(PackageInfo*, uint8* sha1, int32 cn, void* parserCtx) -> bool
	using LoadPackage_t = bool(*)(PackageInfo*, uint8_t*, int32_t, void*);

	// CUtlMemoryGrow takes the CUtlMemory base (i.e. &vec->m_Memory which
	// is &vec->m_Memory.m_pMemory because CUtlMemory's first member IS
	// m_pMemory) and a grow_size.  Returns void* (we don't use it).
	using CUtlMemoryGrow_t = void*(*)(void* /*pMem*/, int /*growSize*/);

	LoadPackage_t      g_pOrigLoadPackage = nullptr;
	CUtlMemoryGrow_t   g_pCUtlMemoryGrow  = nullptr;

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

	// Set true once we've actually appended our AdditionalApps into
	// package 0.  The reconcile must not fire before this (there'd be
	// nothing to reconcile), and the CheckAppOwnership-driven retry
	// path keys off it.
	std::atomic<bool> g_package0Injected{false};

	// DLC appids (discovered from each AddedApp's provisioned appinfo)
	// that must ALSO be injected into package 0's AppIdVec on every
	// load, so the install planner schedules their `dlcappid` depots.
	// Guarded by g_seededMutex.  Merged with config AdditionalApps at
	// inject time.
	std::vector<uint32_t> g_extraAppIds;

	// Force Steam to re-read licenses (and therefore package 0, now
	// holding our injected AdditionalApps) by broadcasting a
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

		const bool notified = user->notifyLicensesUpdated();
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
		g_pCUtlMemoryGrow(&vec.m_Memory, static_cast<int>(toAdd));

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

		const uint32_t appsAdded = appendToVecLocked(
			pPkg->AppIdVec, appIds, g_seededAppIds, "AppIdVec");

		// Resolve depots from the SLSsteam depot-key cache.  Steam's depot
		// eligibility filter looks at pkg.DepotIdVec — if we only added
		// the appid, the install dialog still reports 0 B because no
		// depots are eligible.
		std::unordered_set<uint32_t> appFilter(appIds.begin(), appIds.end());
		const auto depotIds = collectDepotsForApps(appFilter);
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
	// append our AdditionalApps to AppIdVec.
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

		const auto added = g_config.addedAppIds.get();

		std::vector<uint32_t> ids(added.begin(), added.end());
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

		uint32_t addedNow = 0;
		{
			std::lock_guard<std::mutex> lk(g_seededMutex);
			addedNow = injectFullSetLocked(pInfo, ids);
		}

		// After the package-0 AppIdVec actually contains our apps,
		// broadcast a license update so Steam re-reads ownership.
		// Without this the cold cache hangs at "Loading user data".
		// Also run it even when addedNow==0 on the first pass: a
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
		g_pPackage0.store(nullptr, std::memory_order_release);
		g_licenseReconciled.store(false, std::memory_order_release);
		g_package0Injected.store(false, std::memory_order_release);
		std::lock_guard<std::mutex> lk(g_seededMutex);
		g_seededAppIds.clear();
		g_seededDepotIds.clear();
		g_extraAppIds.clear();
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
}
