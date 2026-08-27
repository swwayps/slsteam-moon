#pragma once

// Discovery and classification helpers for config.cpp.
//
// Why this exists
// ---------------
// The managed-app set comes only from the numeric filename stem of each
// `config/stplug-in/*.lua` script plus luaappids.yaml. A script is named
// after its main app (e.g. `275850.lua`); its body lists that app's
// depots/DLC via `addappid(...)`, which must NOT be treated as separate
// main apps.
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

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ConfigDiscovery
{

// Safety valve, NOT a library-size limit.
//
// Steam's package-0 vector and its license/appinfo reconciliation are not
// designed for an effectively unbounded managed set: a bulk copy of tens of
// thousands of scripts (the Skyapi corpus is ~67k) makes the client stall in
// "Loading user data" for minutes. The cap exists only to keep that
// pathological case bootable.
//
// It must stay far above any realistic library. Measured on the Fedora VM with
// the 67k corpus: 1024 managed apps produced an AppIdVec of 1220 and a
// DepotIdVec of 6042, and the UI needed ~174s to settle; 4096 is still an
// order of magnitude below the corpus while leaving normal libraries (even a
// few thousand titles) completely untouched. Users who genuinely exceed it can
// raise `MaxManagedApps` in config.yaml.
//
// Scripts beyond the cap are never deleted: they stay on disk, are reported as
// `ignored-over-limit`, and become eligible as soon as the active set shrinks
// or the limit is raised.
inline constexpr std::size_t kMaxManagedSourceApps = 4096;

struct ManagedSourceSelection
{
	std::unordered_set<uint32_t> active;
	std::size_t discovered = 0;
	std::size_t ignored = 0;
};

inline ManagedSourceSelection selectManagedSources(
	const std::unordered_set<uint32_t>& stplugApps,
	const std::unordered_set<uint32_t>& luaYamlApps,
	const std::unordered_set<uint32_t>& previouslyActive = {},
	std::size_t maxApps = kMaxManagedSourceApps,
	const std::unordered_set<uint32_t>& installedApps = {})
{
	ManagedSourceSelection result;
	std::unordered_set<uint32_t> discovered = stplugApps;
	discovered.insert(luaYamlApps.begin(), luaYamlApps.end());
	result.discovered = discovered.size();
	result.active.reserve(std::min(maxApps, result.discovered));

	const auto addSorted = [&](const std::unordered_set<uint32_t>& candidates,
	                           bool requireDiscovered) {
		std::vector<uint32_t> sorted(candidates.begin(), candidates.end());
		std::sort(sorted.begin(), sorted.end());
		for (const uint32_t appId : sorted)
		{
			if (result.active.size() >= maxApps) break;
			if (appId == 0 ||
			    (requireDiscovered && !discovered.contains(appId))) continue;
			result.active.insert(appId);
		}
	};

	// Priority order, highest first. Everything here is user intent or already
	// working state, so a bulk copy can never evict it:
	//   1. luaappids.yaml  - explicit manual/plugin overrides
	//   2. installed apps  - the titles that actually have content on disk
	//   3. previously active - keeps a working session stable across a reload
	// Only after those does the remaining budget get filled from the scripts.
	// Installed apps must outrank the generic fill: sorting the leftovers by
	// AppID alone favors the oldest low-numbered Valve entries (10, 20, 1290 in
	// the Skyapi corpus), which are large and are almost never what the user
	// added.
	addSorted(luaYamlApps, false);
	addSorted(installedApps, true);
	addSorted(previouslyActive, true);
	addSorted(stplugApps, false);
	result.ignored = result.discovered - result.active.size();
	return result;
}

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

// Return the ids present before a config reload but absent afterwards. A
// sorted result keeps removal side effects deterministic even though the
// config sets themselves are unordered.
inline std::vector<uint32_t> removedAppIds(
    const std::unordered_set<uint32_t>& before,
    const std::unordered_set<uint32_t>& after)
{
	std::vector<uint32_t> removed;
	for (const uint32_t appId : before)
		if (!after.contains(appId)) removed.push_back(appId);
	std::sort(removed.begin(), removed.end());
	return removed;
}

struct ReloadRemovals
{
	std::vector<uint32_t> managed;
	std::vector<uint32_t> active;
	std::vector<uint32_t> managedAdded;
};

// Classify a config reload by source eligibility and active compatibility
// membership separately. An id can leave managed sources while remaining
// active through an installed legacy/Accela compatibility entry; that case
// must invalidate app-scoped cache state without revoking ownership state.
inline ReloadRemovals classifyReloadRemovals(
    const std::unordered_set<uint32_t>& beforeManaged,
    const std::unordered_set<uint32_t>& afterManaged,
    const std::unordered_set<uint32_t>& beforeActive,
    const std::unordered_set<uint32_t>& afterActive)
{
	return ReloadRemovals{
	    .managed = removedAppIds(beforeManaged, afterManaged),
	    .active = removedAppIds(beforeActive, afterActive),
	    .managedAdded = removedAppIds(afterManaged, beforeManaged),
	};
}

struct InstalledApps
{
	std::unordered_set<uint32_t> all;
	std::unordered_set<uint32_t> accela;
};

struct AppIdSets
{
	// Eligible for appinfo acquisition and cache generation.
	std::unordered_set<uint32_t> managed;
	// Installed compatibility entries that receive ownership/package handling
	// but never trigger product-info network requests.
	std::unordered_set<uint32_t> compatibility;
	// Union consumed by ownership and package hooks.
	std::unordered_set<uint32_t> active;
};

inline uint32_t appIdFromManifestName(std::string_view filename)
{
	constexpr std::string_view prefix = "appmanifest_";
	constexpr std::string_view suffix = ".acf";
	if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) return 0;

	const auto digits = filename.substr(
	    prefix.size(), filename.size() - prefix.size() - suffix.size());
	if (digits.empty()) return 0;

	uint64_t value = 0;
	for (char c : digits)
	{
		if (c < '0' || c > '9') return 0;
		value = value * 10 + static_cast<uint64_t>(c - '0');
		if (value > 0xFFFFFFFFull) return 0;
	}
	return static_cast<uint32_t>(value);
}

inline std::string quotedField(const std::string& text, std::string_view key)
{
	const std::string quotedKey = "\"" + std::string(key) + "\"";
	const auto keyPos = text.find(quotedKey);
	if (keyPos == std::string::npos) return {};
	const auto open = text.find('"', keyPos + quotedKey.size());
	if (open == std::string::npos) return {};
	const auto close = text.find('"', open + 1);
	if (close == std::string::npos) return {};
	return text.substr(open + 1, close - open - 1);
}

// Return the default and external steamapps directories for one Steam root.
// Both current and older libraryfolders.vdf locations are accepted.
inline std::vector<std::filesystem::path>
steamAppsRootsFor(const std::filesystem::path& steamRoot)
{
	std::vector<std::filesystem::path> roots;
	const auto addUnique = [&](std::filesystem::path path) {
		path = path.lexically_normal();
		if (std::find(roots.begin(), roots.end(), path) == roots.end())
			roots.push_back(std::move(path));
	};

	addUnique(steamRoot / "steamapps");
	const std::filesystem::path libraryFiles[] = {
		steamRoot / "steamapps" / "libraryfolders.vdf",
		steamRoot / "config" / "libraryfolders.vdf",
	};
	for (const auto& path : libraryFiles)
	{
		std::ifstream file(path);
		if (!file.is_open()) continue;
		std::ostringstream contents;
		contents << file.rdbuf();
		const std::string text = contents.str();

		std::size_t cursor = 0;
		while (cursor < text.size())
		{
			const auto keyPos = text.find("\"path\"", cursor);
			if (keyPos == std::string::npos) break;
			const auto open = text.find('"', keyPos + 6);
			if (open == std::string::npos) break;
			const auto close = text.find('"', open + 1);
			if (close == std::string::npos) break;
			const std::string library = text.substr(open + 1, close - open - 1);
			if (!library.empty()) addUnique(std::filesystem::path(library) / "steamapps");
			cursor = close + 1;
		}
	}
	return roots;
}

// Resolve installed app ids from appmanifest_<id>.acf files whose installdir
// still contains content. Accela entries additionally require the install's
// .DepotDownloader marker, matching Accela's own scanner.
inline InstalledApps
scanInstalledApps(const std::vector<std::filesystem::path>& steamAppsRoots)
{
	InstalledApps result;
	for (const auto& steamapps : steamAppsRoots)
	{
		std::error_code ec;
		std::filesystem::directory_iterator entries(
		    steamapps, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) continue;

		for (const auto& entry : entries)
		{
			if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
			const uint32_t appId = appIdFromManifestName(
			    entry.path().filename().string());
			if (appId == 0) continue;

			std::ifstream manifest(entry.path());
			if (!manifest.is_open()) continue;
			std::ostringstream contents;
			contents << manifest.rdbuf();
			const std::string installDir = quotedField(contents.str(), "installdir");
			const std::filesystem::path relative(installDir);
			if (relative.empty() || relative.is_absolute() || relative.has_parent_path()
			    || relative == "." || relative == "..")
			{
				continue;
			}

			const auto gameDir = steamapps / "common" / relative;
			if (!std::filesystem::is_directory(gameDir, ec) || ec)
			{
				ec.clear();
				continue;
			}
			const bool hasAccelaMarker =
			    std::filesystem::exists(gameDir / ".DepotDownloader", ec) && !ec;
			ec.clear();
			bool hasContent = false;
			std::filesystem::directory_iterator gameEntries(
			    gameDir, std::filesystem::directory_options::skip_permission_denied, ec);
			if (ec) { ec.clear(); continue; }
			for (const auto& gameEntry : gameEntries)
			{
				if (gameEntry.path().filename() != ".DepotDownloader")
				{
					hasContent = true;
					break;
				}
			}
			if (!hasContent) continue;
			result.all.insert(appId);
			if (hasAccelaMarker) result.accela.insert(appId);
		}
	}
	return result;
}

inline AppIdSets classifyAppIds(
    const std::unordered_set<uint32_t>& stplugApps,
    const std::unordered_set<uint32_t>& luaYamlApps,
    const std::unordered_set<uint32_t>& legacyApps,
    const std::unordered_set<uint32_t>& installedApps,
    const std::unordered_set<uint32_t>& accelaApps)
{
	AppIdSets result;
	result.managed.insert(stplugApps.begin(), stplugApps.end());
	result.managed.insert(luaYamlApps.begin(), luaYamlApps.end());
	result.active = result.managed;

	for (uint32_t appId : accelaApps)
	{
		if (!result.managed.contains(appId)) result.compatibility.insert(appId);
		result.active.insert(appId);
	}
	for (uint32_t appId : legacyApps)
	{
		if (!installedApps.contains(appId)) continue;
		if (!result.managed.contains(appId)) result.compatibility.insert(appId);
		result.active.insert(appId);
	}
	return result;
}

} // namespace ConfigDiscovery
