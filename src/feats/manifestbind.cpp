// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifestbind.hpp for design notes.

#include "manifestbind.hpp"

#include "depotkey.hpp"
#include "depotkey_scope.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>


namespace
{
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

	struct Detour
	{
		DepotFn_t    orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t    size = 0;
	};

	Detour g_leaf;     // ProcessDepotManifest
	Detour g_planner;  // PrepareDepotDownload

	bool g_fallbackEnabled = true;


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

	// Find a locally-staged manifest for `depotId` whose gid differs from
	// `planned` (i.e. the LuaTools zip's own manifest).  Picks the most
	// recently written one when several exist.  Returns 0 if none.
	uint64_t findLocalAltGid(const std::string& depotcacheDir,
	                         uint32_t depotId, uint64_t planned)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(depotcacheDir, ec)) return 0;

		const std::string prefix = std::to_string(depotId) + "_";
		uint64_t best = 0;
		long bestMtime = -1;
		for (const auto& entry :
		     std::filesystem::directory_iterator(depotcacheDir, ec))
		{
			if (ec) break;
			if (!entry.is_regular_file(ec)) continue;
			const auto name = entry.path().filename().string();
			if (name.rfind(prefix, 0) != 0) continue;
			const auto dot = name.rfind(".manifest");
			if (dot == std::string::npos || dot + 9 != name.size()) continue;
			const auto gidStr = name.substr(prefix.size(),
			                                dot - prefix.size());
			if (gidStr.empty()) continue;

			uint64_t gid = 0;
			try { gid = std::stoull(gidStr); } catch (...) { continue; }
			if (!gid || gid == planned) continue;

			struct stat st{};
			if (stat(entry.path().c_str(), &st) != 0 || st.st_size <= 0)
			{
				continue;
			}
			if (static_cast<long>(st.st_mtime) > bestMtime)
			{
				bestMtime = static_cast<long>(st.st_mtime);
				best = gid;
			}
		}
		return best;
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
		    /*depotHasPin=*/false);
	}

	// Shared redirect decision: when the planned (public) gid's manifest is
	// not on disk but a different gid for the same depot IS available
	// (depotcache or the persistent ManifestStore), return that local gid;
	// otherwise return the planned gid unchanged.
	uint64_t redirectGid(const char* site, uint32_t appId, uint32_t depotId,
	                     uint64_t manifestId)
	{
		if (!(g_fallbackEnabled && manifestId && depotId
		      && depotInScope(appId, depotId)))
		{
			return manifestId;
		}

		const auto steamRoot = findSteamRoot();
		if (steamRoot.empty()) return manifestId;

		const std::string dc = steamRoot + "/depotcache";

		// Capture whatever manifests are currently staged for this depot
		// into the purge-proof store BEFORE Steam can purge them.
		// Idempotent. (Bulk capture of all of an app's depots happens at
		// PICS install-plan time in pics.cpp; this is belt-and-suspenders.)
		ManifestStore::archiveDepot(depotId);

		// Gate: if the planned (public) gid IS already in depotcache, the
		// providers worked (or it's still staged) -> install it as-is.
		if (manifestOnDisk(dc, depotId, manifestId)) return manifestId;

		// The planned gid is NOT in depotcache.  Case 1 (the common one,
		// e.g. a Proton-switch after Steam purged the windows depot's
		// manifest): we archived this EXACT gid earlier -> restore it from
		// the store so Steam finds it on disk and skips the request-code
		// fetch.  No redirect needed; Steam installs the gid it planned.
		if (ManifestStore::restoreToDepotcache(depotId, manifestId))
		{
			g_pLog->info("ManifestBind[%s]: depot=%u restored planned gid=%llu "
			             "from store (no internet needed)\n",
			             site, depotId,
			             static_cast<unsigned long long>(manifestId));
			return manifestId;
		}

		// Case 2: the planned gid is unavailable anywhere (the live public
		// build is newer than anything we hold).  Fall back to a DIFFERENT
		// gid for the depot -- newest in depotcache, else newest archived
		// in the store (restored into depotcache).
		uint64_t alt = findLocalAltGid(dc, depotId, manifestId);
		if (!alt)
		{
			alt = ManifestStore::bestArchivedGid(depotId, manifestId);
			if (alt) ManifestStore::restoreToDepotcache(depotId, alt);
		}
		if (!alt) return manifestId;

		g_pLog->info(
		    "ManifestBind[%s]: depot=%u public gid=%llu not staged; "
		    "installing local manifest gid=%llu instead (resilience fallback)\n",
		    site, depotId,
		    static_cast<unsigned long long>(manifestId),
		    static_cast<unsigned long long>(alt));
		return alt;
	}

	void* hkProcessDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		const uint64_t useGid = redirectGid("leaf", appId, depotId, manifestId);
		return g_leaf.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	void* hkPrepareDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		const uint64_t useGid = redirectGid("plan", appId, depotId, manifestId);
		return g_planner.orig(ctx, a0C, appId, depotId, useGid, a20);
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

		g_pLog->debug("ManifestBind: leaf=%d planner=%d fallback=%d\n",
		              static_cast<int>(leaf), static_cast<int>(planner),
		              static_cast<int>(g_fallbackEnabled));
		return leaf || planner;
	}

	void remove()
	{
		removeDetour(g_leaf);
		removeDetour(g_planner);
	}
}
