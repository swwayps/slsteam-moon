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
// The primary path stages the public GID (pics.cpp, synchronously, via the
// external request-code providers gmrc.wudrm.com / manifest.steam.run).  When
// those providers are down, the public manifest never lands on disk and the
// install fails.  The LuaTools per-game zip, however, ships its own
// `<depot>_<gid>.manifest` (extracted into depotcache/), which needs no
// request code.  This hook notices that the planned (public) GID is not on
// disk while a different GID for the same depot *is*, and rewrites the gid
// argument so Steam plans/stages the zip's build instead of failing.
//
// The rewrite happens at the planning frame so both the on-disk check (which
// then finds the zip manifest and skips BYldRequestDepotManifest) and the
// subsequent gid-keyed table lookup use the SAME gid.  Redirecting only the
// inner acquisition routine leaves the table keyed by the zip gid while the
// outer frame looks up the original public gid -> NULL deref -> SIGSEGV at
// Reconfiguring (see .kiro/research/manifest-fallback-rootcause.md).
// appinfo.vdf and the PICS product-info buffer are left untouched, so this
// never trips the changelist SHA-1 integrity check that makes splicing
// appinfo.vdf a dead end.
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
}
