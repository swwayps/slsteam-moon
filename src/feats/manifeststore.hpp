// SPDX-License-Identifier: AGPL-3.0-only
//
// ManifestStore — a persistent, purge-proof archive of depot manifests.
//
// Steam purges depotcache/<depot>_<gid>.manifest after a base commit and on
// re-plans.  prewarm normally re-stages purged manifests by FETCHING request
// codes from the external providers — but when the providers are down that
// fails, so a re-plan that needs a purged manifest (e.g. forcing a Proton
// compatibility tool, which re-plans a native-Linux title to its Windows
// depot) dies at "No internet connection".
//
// The LuaTools per-game zip ships a manifest for every depot (linux + windows
// + macos).  The plugin extracts them into depotcache, but Steam can purge
// them at any time.  ManifestStore keeps a copy under
// ~/.config/SLSsteam/manifests/<depot>_<gid>.manifest that Steam never
// touches, and restores them into depotcache on demand — no network needed.
//
// ALL versions are kept (the archive is also the foundation for a future
// manifest-pinning feature: the user picks any archived gid and the
// CDepotDownloadMgr hooks redirect the planned gid to it).
//
// All functions resolve the Steam root internally and degrade to no-ops when
// $HOME / the Steam root / the store dir can't be resolved.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ManifestStore
{
	// Absolute path of the persistent store dir (created on first write).
	// Empty if $HOME is unset.
	std::string dir();

	// Copy every depotcache/<depotId>_*.manifest into the store (skipping
	// ones already archived).  Call before Steam can purge them.
	void archiveDepot(uint32_t depotId);
	void archiveDepots(const std::vector<uint32_t>& depotIds);

	// If store/<depotId>_<gid>.manifest exists and depotcache lacks it,
	// copy it into depotcache so Steam's on-disk check finds it.  Returns
	// true if the manifest is present in depotcache afterwards.
	bool restoreToDepotcache(uint32_t depotId, uint64_t gid);

	// Best archived gid for a depot, excluding `excludeGid` (the planned
	// public gid).  "Best" = most recently archived.  Returns 0 if none.
	uint64_t bestArchivedGid(uint32_t depotId, uint64_t excludeGid);

	// Delete every archived manifest for the given depots (LuaTools
	// remove-game / AdditionalApps pruning).
	void purgeDepots(const std::vector<uint32_t>& depotIds);
}
