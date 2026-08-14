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

#include "../manifest_index.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"
#include "../utils/ManifestFetch.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <atomic>
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
	// CDepotDownloadMgr for the full RE).  Both share this 7-dword cdecl
	// signature:
	//   (ctx, a0C, appId, depotId, uint64 manifestId, a20) -> (eax)
	//
	//   * ProcessDepotManifest — the manifest-acquisition LEAF: builds
	//     "<steamRoot>/depotcache/<depot>_<gid>.manifest", checks it on disk,
	//     calls BYldRequestDepotManifest only when missing.  Redirecting the
	//     gid here makes the on-disk check find the locally-staged (zip)
	//     manifest and skip the request-code fetch (lets a providers-down
	//     install proceed).  5 callers go through it; hooking the leaf covers
	//     them all for the BYld decision.
	//
	//   * PrepareDepotDownload — a LATER pipeline stage (one of those callers)
	//     that, after the leaf returns, looks the depot up in the per-download
	//     table BY the gid it was called with and derefs the per-manifest
	//     state pointer.  Must be redirected too, else it looks up the public
	//     gid the leaf no longer staged -> NULL deref -> SIGSEGV.
	//
	// Both are self-contained PIC.  ProcessDepotManifest's get_pc_thunk is its
	// first instruction (in the relocated tramp -> fixPICThunkCall repairs
	// it); PrepareDepotDownload's is at +5 (not relocated -> fixPICThunkCall
	// is a harmless no-op).
	using DepotFn_t = void*(*)(void*, uint32_t, uint32_t, uint32_t,
	                           uint64_t, uint32_t);

	// FUNC_1141 / CDepotDownloadMgr::BuildDepotDependency — the install-plan
	// consumer.  cdecl, 4 dwords:
	//   (void* ctx, uint32_t flag, CUtlVector<DepotEntry>* depots, uint32_t a3)
	using BuildDepFn_t = void*(*)(void*, uint32_t, void*, uint32_t);

	// DepotEntry layout (matching OpenSteamTool Structs.h):
	//   +0x00 u32 DepotId   +0x04 u32 AppId
	//   +0x08 u64 ManifestGid   +0x10 u64 ManifestSize
	//   +0x18 u32 DlcAppId   +0x1c u8 Lcs   +0x1d u8 bNotNewTarget
	//   +0x1e u8 SharedInstall ; stride 0x20.
	// CUtlVector<DepotEntry>: element base @ +0x00 (m_Memory.m_pMemory),
	//   count (m_Size) @ +0x0c.
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

	// --- DIAGNOSTIC: install-planner runtime trace (env SLSSTEAM_PLAN_TRACE) -
	//
	// Static analysis could not tie the
	// depot-list BUILDER (the LumaCore `BuildDepotDependency` equivalent) back
	// to an external caller — the consumer `sub_FDFD00` (VA 0xfdfd00) appears
	// to have no static caller, no vtable slot, no GOT entry.  The unblocking
	// move is to recover the dynamic CALL CHAIN at the two frames we already
	// hook (ProcessDepotManifest @0xfa8b40, PrepareDepotDownload @0xfa9050).
	//
	// We do NOT use backtrace()/libgcc _Unwind: it SIGABRTs in this 32-bit
	// client when invoked from inside a libmem trampoline (no eh_frame for the
	// tramp frame -> unwinder aborts; confirmed: client Abort at the exact
	// Reconfiguring tick the hook first fired).  Instead we scan our own stack
	// for dwords that point INTO steamclient's mapped range AND are preceded by
	// a `call` instruction (0xE8 rel32 at V-5, or 0xFF /2..3 at V-2/V-3) — a
	// safe, allocation-free, unwinder-free "poor man's backtrace".  Each hit is
	// logged as a steamclient-relative VA (V - base), directly comparable to
	// the ELF VAs.  The consumer sub_FDFD00's frame and, crucially, ITS
	// caller (the builder/scheduler) appear in the scan window.
	//
	// Gated behind the env var so a normal session pays nothing; capped per
	// frame so a many-depot install doesn't flood ~/.SLSsteam.log.
	bool g_planTrace = false;
	std::atomic<int> g_traceLeftLeaf{12};
	std::atomic<int> g_traceLeftPlanner{12};

	// Is `v` a plausible return address: inside steamclient .text and preceded
	// by a call instruction?  Reads only mapped code (safe).
	bool looksLikeRetAddr(uintptr_t v, uintptr_t base, uintptr_t end)
	{
		if (v < base + 16 || v >= end) return false;
		const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
		// call rel32: E8 xx xx xx xx  -> return addr is at insn+5
		if (p[-5] == 0xE8) return true;
		// call r/m32: FF /2 (modrm reg field == 2), 2- or 3-byte forms
		if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;
		if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;
		// call r/m32 with disp8/disp32 + SIB: be lenient, accept FF in window
		if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;
		if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;
		return false;
	}

	void logPlanStack(const char* tag, std::atomic<int>& budget,
	                  uint32_t appId, uint32_t depotId, uint64_t gid)
	{
		if (!g_planTrace) return;
		if (budget.fetch_sub(1) <= 0) return;

		const auto base = reinterpret_cast<uintptr_t>(g_modSteamClient.base);
		const auto end = reinterpret_cast<uintptr_t>(g_modSteamClient.end);

		// Start just above our own frame and scan upward (stack grows down).
		volatile int anchor = 0;
		uintptr_t sp = reinterpret_cast<uintptr_t>(&anchor);
		const uintptr_t scanWords = 320;  // ~1.25 KiB window

		g_pLog->info("PlanTrace[%s]: app=%u depot=%u gid=%llu base=%p sp=%p\n",
		             tag, appId, depotId,
		             static_cast<unsigned long long>(gid),
		             g_modSteamClient.base, reinterpret_cast<void*>(sp));

		int shown = 0;
		for (uintptr_t i = 0; i < scanWords && shown < 24; ++i)
		{
			const uintptr_t slot = sp + i * sizeof(uintptr_t);
			const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(slot);
			if (looksLikeRetAddr(v, base, end))
			{
				g_pLog->info("PlanTrace[%s]:   +0x%lx  steamclient+0x%lx\n",
				             tag,
				             static_cast<unsigned long>(i * sizeof(uintptr_t)),
				             static_cast<unsigned long>(v - base));
				++shown;
			}
		}
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
		return DepotKey::depotInManifestScope(
		    appId   && g_config.isAddedAppId(appId),
		    depotId && g_config.isAddedAppId(depotId),
		    DepotKey::isManagedDepot(depotId),
		    /*depotHasPin=*/g_config.getManifestPin(appId, depotId) != 0);
	}

	// Shared redirect decision: when the planned (public) gid's manifest is
	// not on disk but a different gid for the same depot IS available
	// (depotcache or the persistent ManifestStore), return that local gid;
	// otherwise return the planned gid unchanged.
	uint64_t redirectGid(const char* site, uint32_t appId, uint32_t depotId,
	                     uint64_t manifestId, bool consumeFrozen)
	{
		// Explicit pin: the pin is TARGET-ONLY.  Ensure the
		// pinned manifest is STAGED on disk (so the patched plan and
		// ReconcilePin's reconcile target can fetch it) but DO NOT redirect the
		// gid here.
		//
		// Returning `pin` unconditionally (the old behaviour) contaminated the
		// ACTIVE/installed manifest read: when Steam loads the installed
		// (public) manifest to compute the public->pinned chunk delta, serving
		// the pinned manifest instead made it diff pinned-vs-pinned -> 0 chunks
		// -> a 0-file commit -> the real content (e.g. the build-locked exe a
		// crack validates) was NEVER downloaded, even though Steam recorded the
		// pinned gid and reported "Fully Installed" (confirmed in testing).  The pin belongs ONLY on the
		// TARGET (FUNC_1141 plan DepotEntry + feats/reconcilepin.cpp's reconcile
		// target pass), never on this acquisition/active path.  Leaving the
		// real gid here lets Steam load the genuine public active manifest,
		// compute a true delta against the pinned target, and download it.
		const uint64_t pin = g_config.getManifestPin(appId, depotId);
		if (pin)
		{
			// Stage the pinned manifest if absent (harmless when present: just
			// an on-disk check).  Needed because a pinned build is commonly one
			// the user never installed, so it is NOT in depotcache/the store
			// yet, and Steam's own BYldRequestDepotManifest is "Access Denied"
			// for a managed (non-purchased) app.  Restore from the persistent
			// store if we archived it before, else fetch+stage via ManifestFetch
			// (same request-code provider + CDN path pics.cpp uses) and persist.
			const auto steamRoot = findSteamRoot();
			if (!steamRoot.empty())
			{
				const std::string dc = steamRoot + "/depotcache";
				if (!manifestOnDisk(dc, depotId, pin)
				    && !ManifestStore::restoreToDepotcache(depotId, pin))
				{
					ManifestFetch::fetchManifestBlobSync(pin, depotId);
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

		// The planned gid is NOT in depotcache.  Case 1 (the common one,
		// e.g. a Proton-switch after Steam purged the windows depot's
		// manifest): we archived this EXACT gid earlier -> restore it from
		// the store so Steam finds it on disk and skips the request-code
		// fetch.  No redirect needed; Steam installs the gid it planned.
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
		ManifestFetch::submitManifestBlob(manifestId, appId, depotId);
		const int waitMs = remainingPlanBudgetMs(depotId, manifestId);
		const bool fetched = ManifestFetch::awaitManifestBlobFor(
		    manifestId, depotId, waitMs, /*notifyOnTimeout=*/false);
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

	void* hkProcessDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		logPlanStack("leaf", g_traceLeftLeaf, appId, depotId, manifestId);
		const uint64_t useGid =
		    redirectGid("leaf", appId, depotId, manifestId, false);
		return g_leaf.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	void* hkPrepareDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		logPlanStack("plan", g_traceLeftPlanner, appId, depotId, manifestId);
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
							ManifestFetch::submitManifestBlob(pin, entryAppId, depotId);
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
						g_pLog->info(
						    "ManifestBind[build]: dropping empty depot %u (size 0) from plan\n",
						    depotId);
						continue;
					}

					if (DepotQuarantine::shouldDropManagedDlc(depotId, dlcAppId))
					{
						g_pLog->info(
						    "ManifestBind[build]: omitting quarantined DLC depot=%u "
						    "dlcappid=%u on retry\n",
						    depotId, dlcAppId);
						continue;
					}

					const uint32_t entryAppId =
						*reinterpret_cast<const uint32_t*>(e + kDepotEntryAppIdOff);
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
						    [depotId, pin](int waitMs)
						    {
							return ManifestFetch::awaitManifestBlobFor(
							    pin, depotId, waitMs, /*notifyOnTimeout=*/false);
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
					    && (DepotKey::isManagedDepot(depotId) || pin))
					{
						registerPlanTarget(depotId, targetGid, planDeadline);
						if (planDepotcache.empty()
						    || !manifestOnDisk(
						        planDepotcache, depotId, targetGid))
						{
							ManifestFetch::submitManifestBlob(
							    targetGid, entryAppId, depotId);
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

		if (const char* env = std::getenv("SLSSTEAM_PLAN_TRACE"))
		{
			g_planTrace = !(env[0] == '0' && env[1] == '\0');
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
