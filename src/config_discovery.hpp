#pragma once

// Pure decision layer for AdditionalApps discovery (src/config.cpp).
//
// Why this exists
// ---------------
// The new-version AdditionalApps set is unioned from three sources; the
// PRIMARY one is the numeric filename stem of each `config/stplug-in/*.lua`
// script. A script is named after the app it unlocks (e.g. `275850.lua`);
// its body lists that app's depots/DLC via `addappid(...)`, which must NOT
// be treated as separate main apps.
//
// A regression (commit 9d062c6) additionally rejected any discovered
// main-app id whose numeric value ALSO carried a cached, managed depot key
// (`!DepotKey::isManagedDepot(id)`). That is wrong: single-depot titles
// reuse the app id as their own content depot id, so the script contains a
// keyed `addappid(<appid>, 1, "<key>")` line for the app itself. No Man's
// Sky (275850) is exactly this layout, whereas Green Hell (815370) keys
// only its sibling depots (815371/815372). The guard therefore silently
// dropped No Man's Sky from AdditionalApps and hid it from Steam's library
// entirely, while Green Hell installed fine.
//
// The rule below pins the correct decision: a MAIN-app id (filename stem or
// luaappids.yaml entry) is authoritative and is kept regardless of whether
// the same id is also a managed depot. Managed-depot status only governs
// depot-level manifest hooks; it is irrelevant to app installability.

#include <cstdint>
#include <string_view>

namespace ConfigDiscovery
{

// Parse the app id a stplug-in script encodes through its FILENAME.
// Returns 0 unless `filename` is a purely-numeric "<digits>.lua" name that
// fits in uint32_t (rejects "keys.lua", "275850_backup.lua", ".lua", etc.).
inline uint32_t appIdFromScriptName(std::string_view filename)
{
	const auto dot = filename.rfind('.');
	if (dot == std::string_view::npos) return 0;
	if (filename.substr(dot) != ".lua") return 0;

	const auto stem = filename.substr(0, dot);
	if (stem.empty()) return 0;

	uint64_t v = 0;
	for (char c : stem)
	{
		if (c < '0' || c > '9') return 0;               // non-numeric stem
		v = v * 10 + static_cast<uint64_t>(c - '0');
		if (v > 0xFFFFFFFFull) return 0;                // overflow guard
	}
	return static_cast<uint32_t>(v);
}

// Whether a MAIN-app id discovered from a stplug-in filename stem or from
// luaappids.yaml must be registered in AdditionalApps.
//
// The id is authoritative: it is kept even when the same numeric id also
// carries a cached managed depot key. `idIsAlsoManagedDepot` is accepted
// only to document that it is DELIBERATELY IGNORED here — filtering on it
// hides single-depot titles (e.g. No Man's Sky 275850) from the library.
inline bool keepDiscoveredMainApp(uint32_t appId, bool idIsAlsoManagedDepot)
{
	(void)idIsAlsoManagedDepot;
	return appId > 0;
}

} // namespace ConfigDiscovery
