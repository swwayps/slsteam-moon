// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifestbind.hpp for design notes.

#include "manifestbind.hpp"

#include "depotkey.hpp"
#include "depotkey_scope.hpp"
#include "depotquarantine.hpp"
#include "manageddepotfilter.hpp"
#include "manifestselection.hpp"
#include "manifeststore.hpp"
#include "manifestdonor.hpp"

#include "../manifest_index.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"
#include "../utils/ManifestFetch.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>


namespace
{
	void setGameOfflineStatus(uint32_t appId, bool offline)
	{
		if (!appId) return;
		std::string path = g_config.getDir() + "/offline_" + std::to_string(appId);
		if (offline)
		{
			std::ofstream ofs(path, std::ios::trunc);
			if (ofs.is_open()) ofs << "1";
		}
		else
		{
			std::remove(path.c_str());
		}
	}
	// Two cooperating detours in CDepotDownloadMgr (see patterns.cpp
	// CDepotDownloadMgr for the full RE).  Both share this x86-64 signature:
	//   (mgr, user, appId, depotId, uint64 manifestId, branch) -> rax
	//
	//   * ProcessDepotManifest — the manifest-acquisition LEAF: builds
	//     "<steamRoot>/depotcache/<depot>_<gid>.manifest", checks it on disk,
	//     calls BYldRequestDepotManifest only when missing.  A planned gid that
	//     cannot be staged is redirected to a local archived one, so an offline
	//     install proceeds.  All 5 callers go through it, so the leaf covers
	//     every BYld decision.
	//
	//   * PrepareDepotDownload — a LATER pipeline stage (one of those callers)
	//     that, after the leaf returns, looks the depot up in the per-download
	//     table BY the gid it was called with and derefs the per-manifest
	//     state pointer.  Must be redirected too, else it looks up the public
	//     gid the leaf no longer staged -> NULL deref -> SIGSEGV.
	//
	// PrepareDepotDownload passes its arguments to ProcessDepotManifest
	// unchanged, so both detours see the same gid.
	using DepotFn_t = void*(*)(void*, void*, uint32_t, uint32_t,
	                           uint64_t, void*);

	// FUNC_1141 / CDepotDownloadMgr::BuildDepotDependency — the install-plan
	// consumer.  cdecl, 4 dwords:
	//   (void* ctx, uint32_t flag, CUtlVector<DepotEntry>* depots, uint32_t a3)
	using BuildDepFn_t = void*(*)(void*, uint32_t, void*, uint32_t);

	// DepotEntry layout (matching OpenSteamTool Structs.h):
	//   +0x00 u32 DepotId   +0x04 u32 AppId
	//   +0x08 u64 ManifestGid   +0x10 u64 ManifestSize
	//   +0x18 u32 DlcAppId   +0x1c u8 Lcs   +0x1d u8 bNotNewTarget
	//   +0x1e u8 SharedInstall ; stride 0x20.
	// CUtlVector<DepotEntry>: element base @ +0x00 (memory.base),
	//   count (size) @ +0x0c.
	constexpr size_t kDepotEntryStride = ManagedDepotFilter::kDepotEntryStride;
	constexpr size_t kDepotEntryAppIdOff = 0x04;
	constexpr size_t kDepotEntryGidOff = 0x08;
	constexpr size_t kDepotEntrySizeOff = ManagedDepotFilter::kDepotSizeOff;
	constexpr size_t kDepotEntryDlcAppIdOff = 0x18;
	constexpr size_t kVecBaseOff = 0x00;
	constexpr size_t kVecCapacityOff = 0x04;
	constexpr size_t kVecCountOff = 0x0c;
	// Steam invokes BuildDepotDependency in pairs: flag 0 builds the
	// acquisition target and flag 1 describes the installed side.
	constexpr uint32_t kTargetPlanFlag = 0;

	struct Detour
	{
		DepotFn_t    orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t    size = 0;
	};

	Detour g_leaf;     // ProcessDepotManifest
	Detour g_planner;  // PrepareDepotDownload

	// BuildDepotDependency uses its own typed orig (different arity).
	struct BuildDetour
	{
		BuildDepFn_t orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t    size = 0;
	};
	BuildDetour g_builder;  // CDepotDownloadMgr::BuildDepotDependency

	bool g_fallbackEnabled = true;
	// BuildDepotDependency carries the owning appId in each DepotEntry.  Use
	// that context for pin rewriting; when the field is zero, the structured
	// pin map permits only a unique-owner fallback.  No flattened depot->gid
	// lookup is used, so a shared depot cannot inherit another app's pin.

	// Event-driven manifest staging state. BuildDepotDependency sees the exact
	// depots Steam selected for this plan, starts their bounded background
	// jobs, and gives every depot ONE shared 12-second deadline. The leaf
	// freezes the chosen gid and the later planner consumes that decision, so
	// an exact fetch completing between the two hooks cannot create a
	// gid-keyed table mismatch.
	using PlanClock = std::chrono::steady_clock;
	constexpr int kPlanBudgetMs = 12000;
	constexpr auto kPlanStateTtl = std::chrono::minutes(2);

	struct PlanKey
	{
		uint32_t depotId;
		uint64_t gid;

		bool operator==(const PlanKey& other) const noexcept
		{
			return depotId == other.depotId && gid == other.gid;
		}
	};

	struct PlanKeyHash
	{
		std::size_t operator()(const PlanKey& key) const noexcept
		{
			return std::hash<uint32_t>{}(key.depotId)
			    ^ (std::hash<uint64_t>{}(key.gid) << 1);
		}
	};

	struct PlanState
	{
		PlanClock::time_point deadline{};
		PlanClock::time_point touched{};
		std::optional<uint64_t> frozenGid;
	};

	std::mutex g_planStateLock;
	std::unordered_map<PlanKey, PlanState, PlanKeyHash> g_planStates;

	void prunePlanStatesLocked(PlanClock::time_point now)
	{
		for (auto it = g_planStates.begin(); it != g_planStates.end();)
		{
			if (now - it->second.touched > kPlanStateTtl)
			{
				it = g_planStates.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void registerPlanTarget(uint32_t depotId, uint64_t gid,
	                        PlanClock::time_point deadline)
	{
		if (!depotId || !gid) return;
		const auto now = PlanClock::now();
		std::lock_guard<std::mutex> lk(g_planStateLock);
		prunePlanStatesLocked(now);
		auto& state = g_planStates[{depotId, gid}];
		if (!state.frozenGid)
		{
			if (state.deadline == PlanClock::time_point{}
			    || now >= state.deadline)
			{
				state.deadline = deadline;
			}
			else if (deadline < state.deadline)
			{
				// Concurrent plans for the same depot/gid share the earliest
				// deadline; a later plan must not extend an earlier wait.
				state.deadline = deadline;
			}
		}
		state.touched = now;
	}

	int remainingPlanBudgetMs(uint32_t depotId, uint64_t gid)
	{
		const auto now = PlanClock::now();
		std::lock_guard<std::mutex> lk(g_planStateLock);
		prunePlanStatesLocked(now);
		auto [it, inserted] = g_planStates.try_emplace(
		    PlanKey{depotId, gid},
		    PlanState{now + std::chrono::milliseconds(kPlanBudgetMs), now,
		              std::nullopt});
		if (!inserted) it->second.touched = now;
		if (now >= it->second.deadline) return 0;
		return static_cast<int>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        it->second.deadline - now)
		        .count());
	}

	std::optional<uint64_t> frozenPlanGid(uint32_t depotId, uint64_t gid,
	                                     bool consume)
	{
		std::lock_guard<std::mutex> lk(g_planStateLock);
		auto it = g_planStates.find({depotId, gid});
		if (it == g_planStates.end() || !it->second.frozenGid)
		{
			return std::nullopt;
		}
		const uint64_t frozen = *it->second.frozenGid;
		if (consume) g_planStates.erase(it);
		return frozen;
	}

	uint64_t freezePlanGid(uint32_t depotId, uint64_t plannedGid,
	                       uint64_t chosenGid, bool consume)
	{
		const auto now = PlanClock::now();
		std::lock_guard<std::mutex> lk(g_planStateLock);
		auto& state = g_planStates[{depotId, plannedGid}];
		state.touched = now;
		state.frozenGid = chosenGid;
		if (consume) g_planStates.erase({depotId, plannedGid});
		return chosenGid;
	}

	std::string findSteamRoot()
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};

		const std::string roots[] = {
			std::string(home) + "/.steam/steam",
			std::string(home) + "/.steam/debian-installation",
			std::string(home) + "/.local/share/Steam",
		};
		for (const auto& r : roots)
		{
			struct stat st{};
			if (stat((r + "/steam.sh").c_str(), &st) == 0) return r;
		}
		return {};
	}

	bool manifestOnDisk(const std::string& depotcacheDir,
	                    uint32_t depotId, uint64_t gid)
	{
		const std::string p = depotcacheDir + "/" + std::to_string(depotId)
		                      + "_" + std::to_string(gid) + ".manifest";
		struct stat st{};
		return stat(p.c_str(), &st) == 0 && st.st_size > 0;
	}

	struct LocalManifestCandidate
	{
		uint64_t gid = 0;
		std::filesystem::file_time_type mtime{};
	};

	struct LocalManifestIndex
	{
		bool initialized = false;
		std::string directory;
		std::filesystem::file_time_type directoryMtime{};
		std::unordered_map<uint32_t, std::vector<LocalManifestCandidate>> byDepot;
	};

	std::mutex g_localManifestIndexLock;
	LocalManifestIndex g_localManifestIndex;

	void rebuildLocalManifestIndexLocked(
		const std::string& depotcacheDir,
		std::filesystem::file_time_type directoryMtime)
	{
		LocalManifestIndex next;
		next.initialized = true;
		next.directory = depotcacheDir;
		next.directoryMtime = directoryMtime;

		std::error_code ec;
		for (const auto& entry :
		     std::filesystem::directory_iterator(depotcacheDir, ec))
		{
			if (ec) break;
			ec.clear();
			if (!entry.is_regular_file(ec) || ec) continue;
			const auto parsed = ManifestIndex::parseManifestName(
				entry.path().filename().string());
			if (!parsed) continue;

			ec.clear();
			const auto mtime = entry.last_write_time(ec);
			if (ec) continue;
			next.byDepot[parsed->depotId].push_back(
				{parsed->gid, mtime});
		}

		for (auto& [depotId, candidates] : next.byDepot)
		{
			(void)depotId;
			std::sort(candidates.begin(), candidates.end(),
				[](const LocalManifestCandidate& lhs,
				   const LocalManifestCandidate& rhs)
				{
					if (lhs.mtime != rhs.mtime) return lhs.mtime > rhs.mtime;
					return lhs.gid > rhs.gid;
				});
		}
		g_localManifestIndex = std::move(next);
	}

	// Find a locally-staged manifest for `depotId` whose gid differs from
	// `planned` (i.e. the LuaTools zip's own manifest).  The directory is
	// indexed once and refreshed only when its mtime changes; each lookup then
	// checks at most the newest candidates instead of walking every manifest.
	uint64_t findLocalAltGid(const std::string& depotcacheDir,
	                         uint32_t depotId, uint64_t planned)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(depotcacheDir, ec)) return 0;
		const auto directoryMtime =
			std::filesystem::last_write_time(depotcacheDir, ec);
		if (ec) return 0;

		std::lock_guard<std::mutex> lk(g_localManifestIndexLock);
		if (!g_localManifestIndex.initialized
		    || g_localManifestIndex.directory != depotcacheDir
		    || g_localManifestIndex.directoryMtime != directoryMtime)
		{
			rebuildLocalManifestIndexLocked(depotcacheDir, directoryMtime);
		}

		const auto it = g_localManifestIndex.byDepot.find(depotId);
		if (it == g_localManifestIndex.byDepot.end()) return 0;
		for (const auto& candidate : it->second)
		{
			if (candidate.gid == planned) continue;
			const std::string path = depotcacheDir + "/"
				+ std::to_string(depotId) + "_"
				+ std::to_string(candidate.gid) + ".manifest";
			struct stat st{};
			if (stat(path.c_str(), &st) == 0 && st.st_size > 0)
				return candidate.gid;
		}
		return 0;
	}

	bool depotInScope(uint32_t appId, uint32_t depotId)
	{
		// Only content WE manage.  A merely-observed key (owned game / Proton
		// runtime) must NOT pull the depot in here: redirectGid would archive
		// it into the ManifestStore and could redirect an owned depot to a
		// stale local gid.  The store holds LuaTools depots only.
		return DepotKey::manifestInManagedScope(
		    appId, depotId,
		    /*depotHasPin=*/g_config.getManifestPin(appId, depotId) != 0);
	}

	// Shared redirect decision: when the planned (public) gid's manifest is
	// not on disk but a different gid for the same depot IS available
	// (depotcache or the persistent ManifestStore), return that local gid;
	// otherwise return the planned gid unchanged.
	uint64_t redirectGid(const char* site, uint32_t appId, uint32_t depotId,
	                     uint64_t manifestId, bool consumeFrozen)
	{
		// The pin is target-only (planner + ReconcilePin): stage the pinned manifest,
		// but keep the planned gid so the active read computes a real delta to it.
		const uint64_t pin = g_config.getManifestPin(appId, depotId);
		if (pin)
		{
			// A pinned build is often not on disk, and Steam's own request is denied
			// for a managed app: restore it from the store, else fetch and persist it.
			const auto steamRoot = findSteamRoot();
			if (!steamRoot.empty())
			{
				const std::string dc = steamRoot + "/depotcache";
				if (!manifestOnDisk(dc, depotId, pin)
				    && !ManifestStore::restoreToDepotcache(depotId, pin))
				{
					// Bounded by the plan-wide budget shared with the builder,
					// so leaf and planner calls cannot each wait the full 12 s.
					const bool activeRequest =
						ManifestFetch::isAnyManagedDownloadActive(appId);
					ManifestFetch::submitManifestBlob(pin, appId, depotId, activeRequest);
					(void)ManifestFetch::awaitManifestBlobFor(
					    pin, appId, depotId, remainingPlanBudgetMs(depotId, pin),
					    /*notifyOnTimeout=*/false, activeRequest);
				}
			}
			g_pLog->debug("ManifestBind[%s]: depot=%u pin gid=%llu staged "
			              "(target-only; active read left intact)\n",
			              site, depotId,
			              static_cast<unsigned long long>(pin));
			return manifestId;
		}

		if (!(g_fallbackEnabled && manifestId && depotId
		      && depotInScope(appId, depotId)))
		{
			return manifestId;
		}

		const auto steamRoot = findSteamRoot();
		if (steamRoot.empty()) return manifestId;

		const std::string dc = steamRoot + "/depotcache";

		if (auto frozen =
		        frozenPlanGid(depotId, manifestId, consumeFrozen))
		{
			g_pLog->debug(
			    "ManifestBind[%s]: depot=%u gid=%llu reusing frozen choice=%llu\n",
			    site, depotId, static_cast<unsigned long long>(manifestId),
			    static_cast<unsigned long long>(*frozen));
			return *frozen;
		}

		// Gate: if the exact public gid is already in depotcache, persist this
		// one file without scanning the whole directory and install it as-is.
		if (ManifestStore::isInDepotcache(depotId, manifestId))
		{
			if (ManifestStore::archiveManifest(depotId, manifestId))
			{
				ManifestStore::markPreferredGid(depotId, manifestId);
			}
			return freezePlanGid(
			    depotId, manifestId, manifestId, consumeFrozen);
		}

		// The planned gid is NOT in depotcache.  If the store holds this EXACT
		// gid (the common case, e.g. Steam purges the windows depot's manifest
		// on a Proton switch), restore it so Steam finds it on disk and skips
		// the request-code fetch.  No redirect is needed; Steam installs the
		// gid it planned.
		if (ManifestStore::restoreToDepotcache(depotId, manifestId))
		{
			ManifestStore::markPreferredGid(depotId, manifestId);
			g_pLog->info("ManifestBind[%s]: depot=%u restored planned gid=%llu "
			             "from store (no internet needed)\n",
			             site, depotId,
			             static_cast<unsigned long long>(manifestId));
			return freezePlanGid(
			    depotId, manifestId, manifestId, consumeFrozen);
		}

		// BuildDepotDependency normally started this exact fetch already.
		// submitManifestBlob is deduplicated, so this also covers paths that
		// reached the leaf without the builder hook. Wait only for the time
		// remaining in the plan-wide budget.
		const bool activeRequest =
			ManifestFetch::isAnyManagedDownloadActive(appId);
		ManifestFetch::submitManifestBlob(
			manifestId, appId, depotId, activeRequest);
		const int waitMs = remainingPlanBudgetMs(depotId, manifestId);
		const bool fetched = ManifestFetch::awaitManifestBlobFor(
		    manifestId, appId, depotId, waitMs,
		    /*notifyOnTimeout=*/false,
		    activeRequest);
		if (fetched && ManifestStore::isInDepotcache(depotId, manifestId))
		{
			ManifestStore::markPreferredGid(depotId, manifestId);
			g_pLog->info(
			    "ManifestBind[%s]: depot=%u exact gid=%llu ready from provider\n",
			    site, depotId, static_cast<unsigned long long>(manifestId));
			return freezePlanGid(
			    depotId, manifestId, manifestId, consumeFrozen);
		}

		// Exact gid failed, timed out, or was confirmed absent. Prefer the
		// last successfully observed public gid. Existing stores from before
		// this metadata retain the mtime-based compatibility fallback.
		const uint64_t preferred =
		    ManifestStore::preferredArchivedGid(depotId, manifestId);
		uint64_t legacy = 0;
		if (!preferred)
		{
			legacy = ManifestStore::bestArchivedGid(depotId, manifestId);
			if (!legacy)
			{
				// Last-resort migration for an old install whose LuaTools ZIP
				// manifest exists only in depotcache. This scan runs solely
				// after an exact failure, never once per PICS depot.
				legacy = findLocalAltGid(dc, depotId, manifestId);
				if (legacy
				    && !ManifestStore::archiveManifest(depotId, legacy))
				{
					legacy = 0;
				}
			}
		}

		const auto decision = ManifestSelection::choose(
		    manifestId, ManifestSelection::ExactState::Unavailable,
		    preferred, legacy);
		if (decision.gid == manifestId)
		{
			g_pLog->info(
			    "ManifestBind[%s]: depot=%u exact gid=%llu unavailable and "
			    "no local fallback exists\n",
			    site, depotId, static_cast<unsigned long long>(manifestId));
			return freezePlanGid(
			    depotId, manifestId, manifestId, consumeFrozen);
		}

		if (!ManifestStore::isInDepotcache(depotId, decision.gid)
		    && !ManifestStore::restoreToDepotcache(depotId, decision.gid))
		{
			g_pLog->info(
			    "ManifestBind[%s]: depot=%u fallback gid=%llu could not be staged\n",
			    site, depotId,
			    static_cast<unsigned long long>(decision.gid));
			return freezePlanGid(
			    depotId, manifestId, manifestId, consumeFrozen);
		}

		setGameOfflineStatus(appId, true);

		g_pLog->info(
		    "ManifestBind[%s]: depot=%u public gid=%llu not staged; "
		    "installing local manifest gid=%llu instead (%s fallback)\n",
		    site, depotId,
		    static_cast<unsigned long long>(manifestId),
		    static_cast<unsigned long long>(decision.gid),
		    decision.source == ManifestSelection::ChoiceSource::PreferredLocal
		        ? "last-observed"
		        : "legacy");
		return freezePlanGid(
		    depotId, manifestId, decision.gid, consumeFrozen);
	}

	void* hkProcessDepot(void* ctx, void* a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, void* a20)
	{
		const uint64_t useGid =
		    redirectGid("leaf", appId, depotId, manifestId, false);
		return g_leaf.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	void* hkPrepareDepot(void* ctx, void* a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, void* a20)
	{
		const uint64_t useGid =
		    redirectGid("plan", appId, depotId, manifestId, true);
		return g_planner.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	// CDepotDownloadMgr::BuildDepotDependency — filter the PLAN in place and
	// prefetch exactly the managed or pinned manifests Steam selected.
	//
	// FUNC_1141 receives a populated DepotEntry vector.  DepotEntry::AppId at
	// +0x04 is the owning app context for the app-scoped lookup; a missing app
	// id is eligible only for a unique-owner fallback, never for an ambiguous
	// shared depot.
	//
	// Defensive: bail on an implausible vector (null base / out-of-range count)
	// so a signature/ABI drift degrades to a harmless pass-through.
	void* hkBuildDepot(void* ctx, uint32_t flag, void* depots, uint32_t a3)
	{
		if (depots)
		{
			const auto planDeadline =
			    PlanClock::now() + std::chrono::milliseconds(kPlanBudgetMs);
			const auto planSteamRoot = findSteamRoot();
			const std::string planDepotcache =
			    planSteamRoot.empty() ? std::string{}
			                          : planSteamRoot + "/depotcache";
			const auto p = reinterpret_cast<char*>(depots);
			char* const base = *reinterpret_cast<char* const*>(p + kVecBaseOff);
			const int32_t capacity =
			    *reinterpret_cast<const int32_t*>(p + kVecCapacityOff);
			auto* const countPtr = reinterpret_cast<int32_t*>(p + kVecCountOff);
			const int32_t count = *countPtr;

			if (base && ManifestSelection::validVectorBounds(
			                count, capacity, kDepotEntryStride))
			{
				// Start every first-import pin before waiting on any one of
				// them. The bounded executor may then fetch concurrently while
				// the rewrite loop below joins each job against one shared
				// absolute deadline.
				if (flag == kTargetPlanFlag)
				{
					for (int32_t i = 0; i < count; ++i)
					{
						const char* const e =
						    base + static_cast<size_t>(i) * kDepotEntryStride;
						const uint32_t depotId =
						    *reinterpret_cast<const uint32_t*>(e);
						const uint32_t entryAppId =
						    *reinterpret_cast<const uint32_t*>(e + kDepotEntryAppIdOff);
						const uint64_t pin = g_config.getManifestPinForPlanner(
						    entryAppId, depotId);
						if (pin && !ManifestStore::installedSize(depotId, pin))
						{
							ManifestFetch::submitManifestBlob(
							    pin, entryAppId, depotId,
							    ManifestFetch::isAnyManagedDownloadActive(entryAppId));
						}
					}
				}

				int32_t writeIdx = 0;
				for (int32_t i = 0; i < count; ++i)
				{
					char* e = base + static_cast<size_t>(i) * kDepotEntryStride;
					const uint32_t depotId =
					    *reinterpret_cast<const uint32_t*>(e);
					auto* const sizep =
					    reinterpret_cast<uint64_t*>(e + kDepotEntrySizeOff);
					const uint32_t dlcAppId =
					    *reinterpret_cast<const uint32_t*>(e + kDepotEntryDlcAppIdOff);
					auto* const gidp =
					    reinterpret_cast<uint64_t*>(e + kDepotEntryGidOff);

					if (ManagedDepotFilter::shouldDrop(
					        *sizep, DepotKey::isManagedDepot(depotId)))
					{
						g_pLog->infoOnce(
						    "ManifestBind[build]: dropping empty depot %u (size 0) from plan\n",
						    depotId);
						continue;
					}

					if (DepotQuarantine::shouldDropManagedDlc(depotId, dlcAppId))
					{
						g_pLog->infoOnce(
						    "ManifestBind[build]: omitting quarantined DLC depot=%u "
						    "dlcappid=%u on retry\n",
						    depotId, dlcAppId);
						continue;
					}

					const uint32_t entryAppId =
						*reinterpret_cast<const uint32_t*>(e + kDepotEntryAppIdOff);
					ManifestDonor::observeDepot(entryAppId, depotId, *gidp);
					const uint64_t pin = g_config.getManifestPinForPlanner(
						entryAppId, depotId);
					if (flag == kTargetPlanFlag && pin)
					{
						// Register the pinned target before waiting so every depot in
						// this builder pass shares the same absolute plan deadline.
						// A first-import pin may have no archived manifest yet; join
						// its deduplicated fetch, then retry the size lookup. The pure
						// policy keeps Steam's public gid/size pair intact on every
						// failure or timeout.
						registerPlanTarget(depotId, pin, planDeadline);
						const auto resolved = ManifestSelection::resolvePinnedPair(
						    *gidp, *sizep, pin,
						    remainingPlanBudgetMs(depotId, pin),
						    [depotId, pin]()
						    {
							return ManifestStore::installedSize(depotId, pin);
						    },
						    [depotId, pin, entryAppId](int waitMs)
						    {
							return ManifestFetch::awaitManifestBlobFor(
							    pin, entryAppId, depotId, waitMs,
							    /*notifyOnTimeout=*/false,
							    ManifestFetch::isAnyManagedDownloadActive(entryAppId));
						    });
						if (resolved.pinned)
						{
							if (*gidp != resolved.gid || *sizep != resolved.size)
							{
								g_pLog->info(
								    "ManifestBind[build]: app=%u depot=%u target "
								    "gid=%llu size=%llu -> pinned gid=%llu size=%llu\n",
								    entryAppId, depotId,
								    static_cast<unsigned long long>(*gidp),
								    static_cast<unsigned long long>(*sizep),
								    static_cast<unsigned long long>(pin),
								    static_cast<unsigned long long>(resolved.size));
							}
							*gidp = resolved.gid;
							*sizep = resolved.size;
						}
						else
						{
							g_pLog->debugOnce(
							    "ManifestBind[build]: app=%u depot=%u pinned gid=%llu "
							    "was not ready before the shared deadline; leaving target "
							    "gid=%llu size=%llu\n",
							    entryAppId, depotId,
							    static_cast<unsigned long long>(pin),
							    static_cast<unsigned long long>(*gidp),
							    static_cast<unsigned long long>(*sizep));
						}
					}

					// This is the exact set Steam selected for the real plan,
					// after pin rewriting and size-0 pruning. Kick off only
					// these manifests on the bounded SLSsteam executor. A pin is
					// also sufficient to include a depot whose key was not marked
					// managed by the current discovery pass.
					const uint64_t targetGid = *gidp;
					if (targetGid
					    && DepotKey::manifestInManagedScope(
					        entryAppId, depotId, pin != 0, dlcAppId))
					{
						registerPlanTarget(depotId, targetGid, planDeadline);
						if (planDepotcache.empty()
						    || !manifestOnDisk(
						        planDepotcache, depotId, targetGid))
						{
							ManifestFetch::submitManifestBlob(
							    targetGid, entryAppId, depotId,
							    ManifestFetch::isAnyManagedDownloadActive(entryAppId));
						}
					}

					if (writeIdx != i)
					{
						char* dest = base + static_cast<size_t>(writeIdx) * kDepotEntryStride;
						std::memcpy(dest, e, kDepotEntryStride);
					}
					++writeIdx;
				}
				*countPtr = writeIdx;
			}
			else
			{
				g_pLog->debugOnce(
				    "ManifestBind[build]: implausible depot vector "
				    "(base=%p count=%d capacity=%d); passing through\n",
				    static_cast<void*>(base), count, capacity);
			}
		}
		return g_builder.orig(ctx, flag, depots, a3);
	}

	// Install one detour; returns false (and leaves the Detour cleared) on
	// any failure so a single missing signature degrades to a safe no-op.
	bool installDetour(Detour& d, Pattern_t& pat, void* hookFn)
	{
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: %s pattern not found; that hook disabled\n",
			             pat.name.c_str());
			return false;
		}
		d.addr = pat.address;
		d.size = LM_HookCode(d.addr, reinterpret_cast<lm_address_t>(hookFn),
		                     &d.tramp);
		if (!d.size || d.tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: failed to install %s hook\n",
			             pat.name.c_str());
			d = Detour{};
			return false;
		}
		MemHlp::fixPICThunkCall(pat.name.c_str(), d.addr, d.tramp);
		d.orig = reinterpret_cast<DepotFn_t>(d.tramp);
		g_pLog->debug("ManifestBind: %s detour at %p, tramp at %p\n",
		              pat.name.c_str(),
		              reinterpret_cast<void*>(d.addr),
		              reinterpret_cast<void*>(d.tramp));
		return true;
	}

	void removeDetour(Detour& d)
	{
		if (d.size && d.addr != LM_ADDRESS_BAD && d.tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(d.addr, d.tramp, d.size);
		}
		d = Detour{};
	}

	// Install the BuildDepotDependency detour (distinct orig arity).
	bool installBuilder(Pattern_t& pat, void* hookFn)
	{
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: %s pattern not found; planner patch disabled\n",
			             pat.name.c_str());
			return false;
		}
		g_builder.addr = pat.address;
		g_builder.size = LM_HookCode(g_builder.addr,
		                             reinterpret_cast<lm_address_t>(hookFn),
		                             &g_builder.tramp);
		if (!g_builder.size || g_builder.tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: failed to install %s hook\n",
			             pat.name.c_str());
			g_builder = BuildDetour{};
			return false;
		}
		MemHlp::fixPICThunkCall(pat.name.c_str(), g_builder.addr, g_builder.tramp);
		g_builder.orig = reinterpret_cast<BuildDepFn_t>(g_builder.tramp);
		g_pLog->debug("ManifestBind: %s detour at %p, tramp at %p\n",
		              pat.name.c_str(),
		              reinterpret_cast<void*>(g_builder.addr),
		              reinterpret_cast<void*>(g_builder.tramp));
		return true;
	}
}


namespace ManifestBind
{
	bool setup()
	{
		if (const char* env = std::getenv("SLSSTEAM_MANIFEST_FALLBACK"))
		{
			g_fallbackEnabled = !(env[0] == '0' && env[1] == '\0');
		}

		// Both hooks cooperate; install independently so one missing
		// signature doesn't disable the other.  The leaf alone lets the
		// install skip BYld but crashes the planner's table lookup; the
		// planner alone never runs because BYld aborts first.  Both together
		// keep the gid consistent end-to-end.
		const bool leaf = installDetour(
		    g_leaf, Patterns::CDepotDownloadMgr::ProcessDepotManifest,
		    reinterpret_cast<void*>(&hkProcessDepot));
		const bool planner = installDetour(
		    g_planner, Patterns::CDepotDownloadMgr::PrepareDepotDownload,
		    reinterpret_cast<void*>(&hkPrepareDepot));

		// BuildDepotDependency applies each entry's app-scoped ManifestPin before
		// the original planner consumes the vector. ReconcilePin also patches the
		// target vectors used by post-commit comparison, keeping both paths aligned.
		const bool builder = installBuilder(
		    Patterns::CDepotDownloadMgr::BuildDepotDependency,
		    reinterpret_cast<void*>(&hkBuildDepot));

		g_pLog->debug("ManifestBind: leaf=%d planner=%d builder=%d fallback=%d\n",
		              static_cast<int>(leaf), static_cast<int>(planner),
		              static_cast<int>(builder),
		              static_cast<int>(g_fallbackEnabled));
		return leaf || planner || builder;
	}

	void remove()
	{
		removeDetour(g_leaf);
		removeDetour(g_planner);
		if (g_builder.size && g_builder.addr != LM_ADDRESS_BAD
		    && g_builder.tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_builder.addr, g_builder.tramp, g_builder.size);
		}
		g_builder = BuildDetour{};
	}
}
