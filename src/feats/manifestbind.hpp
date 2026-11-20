// SPDX-License-Identifier: AGPL-3.0-only
//
// Manifest-bind feature — resilience fallback for installing AdditionalApps.
//
// Hooks CDepotDownloadMgr's depot-download setup function (the planning-level
// frame that invokes the per-depot manifest acquisition routine AND then
// looks the depot up in the per-download table by gid; see patterns.cpp
// PrepareDepotDownload and feats/manifestbind.cpp for the exact RE).
//
// Why: installing an AdditionalApp needs the depot manifest staged on disk.
// The planned (public) GID comes from the ManifestStore or the manifest
// archive (ManifestFetch).  When neither has it, a local manifest of the same
// depot (the store's last-observed GID, or the `<depot>_<gid>.manifest` a
// LuaTools zip put in depotcache/) is installed instead: this hook rewrites
// the gid argument so Steam plans/stages that build instead of failing.
//
// The rewrite happens at the planning frame so both the on-disk check (which
// then finds the zip manifest and skips BYldRequestDepotManifest) and the
// subsequent gid-keyed table lookup use the SAME gid.  Redirecting only the
// inner acquisition routine leaves the table keyed by the zip gid while the
// outer frame looks up the original public gid -> NULL deref -> SIGSEGV at
// Reconfiguring.
// appinfo.vdf and the PICS product-info buffer are left untouched, so this
// never trips the changelist SHA-1 integrity check that makes splicing
// appinfo.vdf non-viable.
//
// Gated on SLSSTEAM_MANIFEST_FALLBACK (default on; "0" disables).  Conceptual
// sibling of OpenSteamTool / LumaCore manifest pinning, adapted for Linux
// i386 and used as a conditional fallback rather than an unconditional pin.

#pragma once

namespace ManifestBind
{
	// Resolve the PrepareDepotDownload pattern and install the detour.
	// Returns false (feature disabled, safe no-op) if the pattern did not
	// resolve or the hook could not be placed.
	bool setup();

	// Tear the detour back down (called from Hooks::remove).
	void remove();
	// True while the BuildDepotDependency detour is placed, i.e. the
	// planner itself starts the planned manifest fetches.
	bool builderInstalled();
}
