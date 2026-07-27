// SPDX-License-Identifier: AGPL-3.0-only
//
// ManifestStore — a persistent, purge-proof archive of depot manifests.
//
// Steam purges depotcache/<depot>_<gid>.manifest after a base commit and on
// re-plans. ManifestStore is the durable source: downloaded manifests are
// published here before depotcache, and the download hooks restore the exact
// planned gid on demand.
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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ManifestStore
{
	using ArchivedGidIndex = std::unordered_map<std::uint32_t, std::uint64_t>;

	// Absolute path of the persistent store dir (created on first write).
	// Empty if $HOME is unset.
	std::string dir();

	// Copy every depotcache/<depotId>_*.manifest into the store (skipping
	// ones already archived). Compatibility path for legacy installs; normal
	// operation uses archiveManifest/publishDownloadedManifest and never scans.
	void archiveDepot(uint32_t depotId);
	void archiveDepots(const std::vector<uint32_t>& depotIds);

	// Archive one exact depotcache file without scanning the directory.
	bool archiveManifest(uint32_t depotId, uint64_t gid);

	// Publish a freshly downloaded/validated temporary manifest to the
	// persistent store FIRST and then materialize it in depotcache. The caller
	// decides whether this gid represents a public observation; pinned
	// manifests must not replace the preferred-public marker.
	bool publishDownloadedManifest(uint32_t depotId, uint64_t gid,
	                               const std::string& sourcePath);

	// Cheap exact checks. A file is ready only when it has the expected Steam
	// manifest magic; non-empty corrupt files are not valid fallbacks.
	bool isInDepotcache(uint32_t depotId, uint64_t gid);
	bool isArchived(uint32_t depotId, uint64_t gid);

	// ManifestGid and ManifestSize identify one target and must be changed as
	// one pair. Cache cb_disk_original by (depot, gid), consulting the durable
	// store first and depotcache as a compatibility source. Repeated planner and
	// reconcile passes never re-read a manifest after a successful parse.
	std::optional<uint64_t> installedSize(uint32_t depotId, uint64_t gid);
	void cacheInstalledSize(uint32_t depotId, uint64_t gid, uint64_t size);

	// If store/<depotId>_<gid>.manifest exists and depotcache lacks it,
	// copy it into depotcache so Steam's on-disk check finds it.  Returns
	// true if the manifest is present in depotcache afterwards.
	bool restoreToDepotcache(uint32_t depotId, uint64_t gid);

	// Record/read the last exact public gid that was successfully available.
	// This defines fallback recency explicitly; manifest gids are opaque and
	// must never be compared numerically.
	bool markPreferredGid(uint32_t depotId, uint64_t gid);
	uint64_t preferredArchivedGid(uint32_t depotId, uint64_t excludeGid);

	// Best archived gid for a depot, excluding `excludeGid` (the planned
	// public gid). Legacy compatibility fallback when no preferred metadata
	// exists. "Best" = most recently archived. Returns 0 if none.
	uint64_t bestArchivedGid(uint32_t depotId, uint64_t excludeGid);

	// Read-only observation index for source fingerprinting. The archive is
	// scanned once and each depot maps to its newest valid artifact by mtime.
	// Install-time selection continues to use bestArchivedGid() unchanged.
	ArchivedGidIndex archivedGidIndex();

	// Delete every archived manifest for the given depots (LuaTools
	// remove-game / AdditionalApps pruning).
	void purgeDepots(const std::vector<uint32_t>& depotIds);
}
