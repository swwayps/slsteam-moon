// SPDX-License-Identifier: AGPL-3.0-only
//
// Post-prune validation for provisioned appinfo.  A depots map can remain
// structurally present after every downloadable depot was removed because it
// still contains metadata (branches, baselanguages, ...) or virtual DLC
// ownership entries.  Persisting that record leaves Steam with an owned app
// that has no installable content.

#pragma once

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>

namespace AppInfoProvision
{

// True when `body.depots` contains at least one numeric depot with manifest
// data.  Non-numeric metadata and virtual DLC entries (`dlcappid` without a
// manifests block) do not represent downloadable content.
inline bool hasUsableContentDepot(const YAML::Node& body)
{
	if (!body || !body.IsMap()) return false;

	const YAML::Node depots = body["depots"];
	if (!depots || !depots.IsMap()) return false;

	for (auto it = depots.begin(); it != depots.end(); ++it)
	{
		std::string key;
		try { key = it->first.as<std::string>(); }
		catch (...) { continue; }

		if (key.empty()) continue;
		bool numeric = true;
		for (char c : key)
		{
			if (c < '0' || c > '9') { numeric = false; break; }
		}
		if (!numeric) continue;

		const YAML::Node depot = it->second;
		if (!depot || !depot.IsMap()) continue;
		const YAML::Node manifests = depot["manifests"];
		if (!manifests || !manifests.IsMap()) continue;

		for (auto manifest = manifests.begin(); manifest != manifests.end();
		     ++manifest)
		{
			const YAML::Node branch = manifest->second;
			if (!branch || !branch.IsMap() || !branch["gid"]) continue;
			try
			{
				if (branch["gid"].as<uint64_t>() != 0) return true;
			}
			catch (...) {}
		}
	}

	return false;
}

} // namespace AppInfoProvision
