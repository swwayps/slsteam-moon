// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifestid.hpp for design notes.

#include "manifestid.hpp"

#include "../config.hpp"
#include "../config_discovery.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace ManifestId
{

namespace
{

// In-memory catalog (depotId -> gid).  Loaded lazily; written through.
std::mutex g_catalogMu;
std::mutex g_importMu;
std::map<uint32_t, std::string> g_catalog;
AppDepotIndex g_appDepotIndex;
bool g_catalogLoaded = false;

std::vector<std::string> steamPathCandidates()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	return {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
}

std::string findSteamRoot()
{
	for (const auto& candidate : steamPathCandidates())
	{
		if (std::filesystem::exists(candidate + "/steam.sh"))
		{
			return candidate;
		}
	}
	return {};
}

// Reload the catalog from disk into the in-memory map.  Caller holds
// g_catalogMu.
void loadCatalogLocked()
{
	if (g_catalogLoaded) return;
	g_catalogLoaded = true;

	const auto dir = getCatalogDir();
	if (!std::filesystem::is_directory(dir)) return;

	static const std::regex fileRe("manifestid_(\\d+)\\.yaml");
	for (const auto& entry : std::filesystem::directory_iterator(dir))
	{
		if (!entry.is_regular_file()) continue;
		const auto name = entry.path().filename().string();
		std::smatch m;
		if (!std::regex_match(name, m, fileRe)) continue;

		// Defensive: skip empty/truncated files outright.  A prior
		// crash-mid-write could leave a zero-byte placeholder; YAML
		// parsing would lob a TypedBadConversion that we'd catch,
		// but the noise isn't worth it.
		std::error_code ec;
		const auto sz = std::filesystem::file_size(entry.path(), ec);
		if (ec || sz == 0) continue;

		try
		{
			auto node = YAML::LoadFile(entry.path().string());
			if (!node["depotId"] || !node["gid"]) continue;
			const auto depotId = node["depotId"].as<uint32_t>();
			const auto gid     = node["gid"].as<std::string>();
			if (depotId && !gid.empty())
			{
				g_catalog[depotId] = gid;
			}
		}
		catch (const std::exception& e)
		{
			g_pLog->debug("ManifestId: failed to load %s: %s\n",
			              name.c_str(), e.what());
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Disk paths
// ---------------------------------------------------------------------------

std::string getCatalogDir()
{
	std::stringstream ss;
	ss << g_config.getDir().c_str() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir.c_str()))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir.c_str(), ec);
	}
	return dir;
}

std::string getCatalogPath(uint32_t depotId)
{
	std::stringstream ss;
	ss << getCatalogDir().c_str() << "/manifestid_" << depotId << ".yaml";
	return ss.str();
}

// ---------------------------------------------------------------------------
// Catalog API
// ---------------------------------------------------------------------------

std::string getPinnedGid(uint32_t depotId)
{
	std::lock_guard<std::mutex> lk(g_catalogMu);
	loadCatalogLocked();
	auto it = g_catalog.find(depotId);
	if (it == g_catalog.end()) return {};
	return it->second;
}

std::vector<uint32_t> getExclusiveDepotsForApp(uint32_t appId)
{
	std::lock_guard<std::mutex> lk(g_catalogMu);
	return g_appDepotIndex.exclusiveDepotsForApp(appId);
}

std::vector<uint32_t> forgetApp(uint32_t appId)
{
	if (appId == 0) return {};
	std::lock_guard<std::mutex> lk(g_catalogMu);
	return g_appDepotIndex.releaseApp(appId);
}

bool savePin(uint32_t depotId, const std::string& gid)
{
	if (!depotId || gid.empty()) return false;
	// Sanity: GIDs are decimal uint64 strings.  Reject anything else
	// to keep YAML parsing safe and to surface garbage early.
	for (char c : gid)
	{
		if (c < '0' || c > '9') return false;
	}

	const auto path = getCatalogPath(depotId);

	// Skip write if file already has the same value.
	if (std::filesystem::exists(path.c_str()))
	{
		try
		{
			auto node = YAML::LoadFile(path);
			if (node["depotId"].as<uint32_t>() == depotId &&
			    node["gid"].as<std::string>() == gid)
			{
				std::lock_guard<std::mutex> lk(g_catalogMu);
				g_catalog[depotId] = gid;
				g_catalogLoaded = true;
				return true;
			}
		}
		catch (...) { /* fall through to rewrite */ }
	}

	YAML::Emitter em;
	em << YAML::BeginMap;
	em << YAML::Key << "depotId" << YAML::Value << depotId;
	em << YAML::Key << "gid"     << YAML::Value << gid;
	em << YAML::EndMap;

	{
		std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("ManifestId: cannot write %s\n", path.c_str());
			return false;
		}
		ofs.write(em.c_str(), em.size());
	}  // close + flush before any reader sees the file

	// Update the in-memory catalog directly.  We don't need to
	// re-iterate the cache directory: the YAML we just wrote is the
	// authoritative entry for `depotId`, and any stale catalog state
	// is irrelevant because every saver path goes through this
	// function.  This also avoids the trap of loadCatalogLocked()
	// racing with a not-yet-flushed ofstream from this very call.
	std::lock_guard<std::mutex> lk(g_catalogMu);
	g_catalog[depotId] = gid;
	g_catalogLoaded = true;
	return true;
}

namespace
{
void retireDepotsLocked(const std::vector<uint32_t>& depotIds)
{
	loadCatalogLocked();
	for (const uint32_t depotId : depotIds)
	{
		if (depotId == 0) continue;
		const auto path = getCatalogPath(depotId);
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec)
		{
			g_pLog->debug("ManifestId: cannot retire stale pin %s: %s\n",
			              path.c_str(), ec.message().c_str());
			continue;
		}
		g_catalog.erase(depotId);
	}
}
} // namespace

void retireDepots(const std::vector<uint32_t>& depotIds)
{
	if (depotIds.empty()) return;
	std::lock_guard<std::mutex> lk(g_catalogMu);
	retireDepotsLocked(depotIds);
}

// ---------------------------------------------------------------------------
// Importer
// ---------------------------------------------------------------------------

void importLuaScripts()
{
	std::lock_guard<std::mutex> importLock(g_importMu);
	// Re-scan on every call so a script added or re-added while Steam is
	// running is imported without a restart. savePin() is idempotent and each
	// script's AppDepotIndex relation is reconciled, so repeated startup and
	// hot-add scans are safe.
	const auto steamRoot = findSteamRoot();
	if (steamRoot.empty()) return;

	const auto stplug = steamRoot + "/config/stplug-in";
	if (!std::filesystem::exists(stplug.c_str())) return;

	// `setManifestid(<depotId>, "<gid>")`
	// `setManifestid(<depotId>, "<gid>", <something>)`
	//
	// Tail args (e.g. priority hints) appear in some community
	// scripts; we ignore them.
	static const std::regex setManifestRe(
		"setManifestid\\s*\\(\\s*(\\d+)\\s*,\\s*\"([0-9]+)\"\\s*"
		"(?:,\\s*[^)]*)?\\)"
	);

	int imported = 0;
	const auto managedApps = g_config.managedAppIds.get();
	for (const auto& entry : std::filesystem::directory_iterator(stplug))
	{
		if (!entry.is_regular_file()) continue;
		const auto& path = entry.path();
		if (path.extension() != ".lua") continue;
		const uint32_t appId = ConfigDiscovery::appIdFromScriptName(
			path.filename().string());
		if (appId == 0 || !managedApps.contains(appId)) continue;

		std::ifstream ifs(path);
		if (!ifs.is_open()) continue;

		// Process line by line so we can strip Lua line comments
		// (`-- ...`).  A previous version regex-scanned the whole file
		// and happily imported pins from commented-out lines like
		// `--setManifestid(638511,"...")`, which then pinned a stale
		// GID that no longer matched what Steam requested — the
		// install would hang.  Strip everything from the first `--`
		// on each line before matching.
		std::vector<uint32_t> scriptDepots;
		std::string line;
		while (std::getline(ifs, line))
		{
			const auto commentPos = line.find("--");
			if (commentPos != std::string::npos)
			{
				line.erase(commentPos);
			}

			auto begin = std::sregex_iterator(line.begin(), line.end(),
			                                  setManifestRe);
			auto end = std::sregex_iterator();
			for (auto it = begin; it != end; ++it)
			{
				const uint32_t depotId =
					static_cast<uint32_t>(std::stoul((*it)[1].str()));
				const std::string gid = (*it)[2].str();
				if (appId != 0) scriptDepots.push_back(depotId);
				if (savePin(depotId, gid)) ++imported;
			}
		}

		// Replace the complete relation set only after the script was read
		// successfully. This removes depots deleted from an edited script while
		// preserving shared ownership for depots still referenced by others.
		if (appId != 0)
		{
			std::lock_guard<std::mutex> lk(g_catalogMu);
			const auto released =
				g_appDepotIndex.replaceApp(appId, scriptDepots);
			retireDepotsLocked(released);
		}
	}
	if (imported > 0)
	{
		g_pLog->infoOnce("ManifestId: imported %d Lua-script manifest pins from %s\n",
		             imported, stplug.c_str());
	}
}

} // namespace ManifestId
