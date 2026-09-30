// SPDX-License-Identifier: AGPL-3.0-only
//
// Manifest GID pinning feature.
//
// Background: a Lua script in `<Steam>/config/stplug-in/` may carry
//
//     setManifestid(<depotId>, "<gid>")
//
// alongside `addappid(...)`.  Without `setManifestid`, the depot is
// installed at whatever `manifests.public.gid` PICS happens to ship
// at the moment of install — which is fine for "I want the latest"
// but breaks reproducibility, version-pinned mods, and any flow that
// expects a specific build.
//
// This feature ingests `setManifestid` calls into a depot-id keyed
// catalog and rewrites the manifests block of the PICS wire-format
// app buffer so Steam's installer picks our pinned GID instead of
// upstream's `public`.
//
// Catalog layout (mirrors DepotKey):
//   <SLSsteam config dir>/cache/manifestid_<depotId>.yaml
//     ---
//     depotId: <uint32>
//     gid: "<numeric-string>"
//
// Application points:
//   - Importer runs at startup alongside DepotKey::importLuaScripts.
//   - Buffer rewrite runs inside PICS::recvProductInfoResponse, just
//     before persistAppBuffer, only for AdditionalApps.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ManifestId
{
	// Tracks which stplug-in script owns each manifest pin. A depot can be
	// referenced by multiple apps, so removing one app only releases a depot
	// when no other app still owns the relation.
	class AppDepotIndex
	{
	public:
		void add(uint32_t appId, uint32_t depotId)
		{
			if (appId == 0 || depotId == 0) return;
			appDepots_[appId].insert(depotId);
			depotApps_[depotId].insert(appId);
		}

		std::vector<uint32_t> replaceApp(
			uint32_t appId, const std::vector<uint32_t>& depots)
		{
			if (appId == 0) return {};

			std::set<uint32_t> desired;
			for (const uint32_t depotId : depots)
				if (depotId != 0) desired.insert(depotId);

			std::vector<uint32_t> released;
			const auto currentIt = appDepots_.find(appId);
			if (currentIt != appDepots_.end())
			{
				for (const uint32_t depotId : currentIt->second)
				{
					if (desired.contains(depotId)) continue;
					auto owners = depotApps_.find(depotId);
					if (owners == depotApps_.end())
					{
						released.push_back(depotId);
						continue;
					}
					if (owners->second.size() == 1)
						released.push_back(depotId);
					owners->second.erase(appId);
					if (owners->second.empty()) depotApps_.erase(owners);
				}
			}

			if (desired.empty())
			{
				appDepots_.erase(appId);
				return released;
			}

			appDepots_[appId] = desired;
			for (const uint32_t depotId : desired)
				depotApps_[depotId].insert(appId);
			return released;
		}

		std::vector<uint32_t> depotsForApp(uint32_t appId) const
		{
			std::vector<uint32_t> out;
			const auto it = appDepots_.find(appId);
			if (it == appDepots_.end()) return out;
			out.assign(it->second.begin(), it->second.end());
			return out;
		}

		std::vector<uint32_t> exclusiveDepotsForApp(uint32_t appId) const
		{
			std::vector<uint32_t> out;
			const auto it = appDepots_.find(appId);
			if (it == appDepots_.end()) return out;
			for (const uint32_t depotId : it->second)
			{
				const auto owners = depotApps_.find(depotId);
				if (owners != depotApps_.end() && owners->second.size() == 1)
					out.push_back(depotId);
			}
			return out;
		}

		std::vector<uint32_t> releaseApp(uint32_t appId)
		{
			const auto it = appDepots_.find(appId);
			if (it == appDepots_.end()) return {};
			const auto released = exclusiveDepotsForApp(appId);
			for (const uint32_t depotId : it->second)
			{
				auto owners = depotApps_.find(depotId);
				if (owners == depotApps_.end()) continue;
				owners->second.erase(appId);
				if (owners->second.empty()) depotApps_.erase(owners);
			}
			appDepots_.erase(it);
			return released;
		}

	private:
		std::map<uint32_t, std::set<uint32_t>> appDepots_;
		std::map<uint32_t, std::set<uint32_t>> depotApps_;
	};

	// Disk paths.
	std::string getCatalogDir();
	std::string getCatalogPath(uint32_t depotId);

	// Returns the pinned GID for `depotId`, or empty string if none.
	std::string getPinnedGid(uint32_t depotId);

	// Persist a (depotId, gid) pair to the catalog.  Idempotent.
	bool savePin(uint32_t depotId, const std::string& gid);


	// Return manifest depots that are referenced only by `appId`.  These are
	// the only depot-scoped catalog files safe to quarantine during removal.
	std::vector<uint32_t> getExclusiveDepotsForApp(uint32_t appId);

	// Release the app's relation after its cache cleanup completed. Returns
	// the depot ids that became unowned by any remaining app.
	std::vector<uint32_t> forgetApp(uint32_t appId);

	// Importer: scans `<Steam>/config/stplug-in/*.lua`, extracts every
	// `setManifestid(<depotId>, "<gid>")` and ingests into the catalog.
	void importLuaScripts();
}
