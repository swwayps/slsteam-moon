// SPDX-License-Identifier: AGPL-3.0-only
//
// ManifestPins — pure container logic for the manifest-pinning feature
// A "pin" locks a depot to a specific manifest gid the
// user has archived; CConfig parses config.yaml's `ManifestPins:` map into
// the structured PinMap and derives a flattened depot->gid index (the
// redirect lookup) plus a locked-app set (the update-lock).  Kept free of
// yaml/globals/I/O so it is host-unit-testable (tools/test_manifestpins.cpp).

#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ManifestPins
{
	struct AppPins
	{
		bool locked = false;
		uint32_t buildId = 0; // pinned build NUMBER (0 = none); GetAppBuildId
		std::unordered_map<uint32_t, uint64_t> depots; // depot -> gid
	};

	// appid -> its pins
	using PinMap = std::unordered_map<uint32_t, AppPins>;

	// Union every app's depot->gid into one index.  A depot can be present in
	// more than one app after malformed or overlapping configuration.  Keep a
	// shared gid when it is identical, but remove conflicting ownership from
	// the fallback index instead of letting iteration order choose a winner.
	inline std::unordered_map<uint32_t, uint64_t> flattenDepots(const PinMap& pins)
	{
		std::unordered_map<uint32_t, uint64_t> flat;
		std::unordered_set<uint32_t> ambiguous;
		for (const auto& [appId, app] : pins)
		{
			if (!appId) continue;
			for (const auto& [depotId, gid] : app.depots)
			{
				if (!depotId || !gid) continue;
				if (ambiguous.contains(depotId)) continue;

				auto [it, inserted] = flat.emplace(depotId, gid);
				if (!inserted && it->second != gid)
				{
					flat.erase(it);
					ambiguous.insert(depotId);
				}
			}
		}
		return flat;
	}

	inline std::unordered_set<uint32_t> lockedAppSet(const PinMap& pins)
	{
		std::unordered_set<uint32_t> locked;
		for (const auto& [appId, app] : pins)
		{
			if (!appId || !app.locked) continue;
			for (const auto& [depotId, gid] : app.depots)
			{
				if (depotId && gid)
				{
					locked.insert(appId);
					break;
				}
			}
		}
		return locked;
	}

	// Resolve a pin only within its owning app.  This is the authoritative
	// lookup for consumers that know the app context; it never consults the
	// flattened fallback and therefore cannot cross-inherit another app's pin.
	inline uint64_t getPin(const PinMap& pins, uint32_t appId, uint32_t depotId)
	{
		if (!appId || !depotId) return 0ULL;
		const auto appIt = pins.find(appId);
		if (appIt == pins.end()) return 0ULL;
		const auto depotIt = appIt->second.depots.find(depotId);
		return depotIt == appIt->second.depots.end() ? 0ULL : depotIt->second;
	}

	inline uint64_t getPinForContext(const PinMap& pins, uint32_t appId,
	                                uint32_t depotId)
	{
		if (!appId || !depotId) return 0ULL;
		return getPin(pins, appId, depotId);
	}

	inline uint64_t getPinForUniqueOwner(const PinMap& pins, uint32_t depotId)
	{
		if (!depotId) return 0ULL;
		uint64_t pin = 0ULL;
		bool found = false;
		for (const auto& [appId, app] : pins)
		{
			if (!appId) continue;
			const auto depotIt = app.depots.find(depotId);
			if (depotIt == app.depots.end() || depotIt->second == 0ULL) continue;
			if (found) return 0ULL;
			found = true;
			pin = depotIt->second;
		}
		return found ? pin : 0ULL;
	}

	// Planner entries normally carry their owning app id.  If a legacy client
	// leaves that field empty, only a depot with one configured owner may use a
	// depot-only fallback; ambiguous ownership is deliberately not resolved.
	inline uint64_t getPinForPlannerEntry(const PinMap& pins, uint32_t appId,
	                                      uint32_t depotId)
	{
		return appId ? getPinForContext(pins, appId, depotId)
		             : getPinForUniqueOwner(pins, depotId);
	}

	// Legacy/global lookup retained for hooks that do not have app context.
	// flattenDepots omits conflicting entries, so this fallback is safe.
	inline uint64_t getPin(const std::unordered_map<uint32_t, uint64_t>& flat,
	                       uint32_t depotId)
	{
		if (!depotId) return 0ULL;
		const auto it = flat.find(depotId);
		return it == flat.end() ? 0ULL : it->second;
	}

	inline bool isLocked(const std::unordered_set<uint32_t>& locked,
	                     uint32_t appId)
	{
		return locked.count(appId) != 0;
	}

	inline void purgeApps(PinMap& pins,
	                      const std::unordered_set<uint32_t>& dropApps)
	{
		for (uint32_t appId : dropApps) pins.erase(appId);
	}

	inline void purgeOrphans(PinMap& pins,
	                         const std::unordered_set<uint32_t>& keepApps)
	{
		for (auto it = pins.begin(); it != pins.end(); )
		{
			if (keepApps.count(it->first) == 0) it = pins.erase(it);
			else ++it;
		}
	}
}
