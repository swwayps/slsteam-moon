// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifeststore.hpp for design notes.

#include "manifeststore.hpp"

#include "../globals.hpp"
#include "../log.hpp"

#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
	// Resolve the active Steam data root (the dir that holds steam.sh and
	// depotcache/).  Layout-independent across distros.  Empty if not found.
	std::string steamRoot()
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};
		const std::string roots[] = {
			std::string(home) + "/.steam/steam",
			std::string(home) + "/.steam/debian-installation",
			std::string(home) + "/.local/share/Steam",
		};
		for (const auto& r : roots)
		{
			struct stat st{};
			if (stat((r + "/steam.sh").c_str(), &st) == 0) return r;
		}
		return {};
	}

	// Parse "<depotId>_<gid>.manifest" -> gid, or 0 if the name doesn't
	// match the expected shape for `prefix` ("<depotId>_").
	uint64_t gidFromName(const std::string& name, const std::string& prefix)
	{
		if (name.rfind(prefix, 0) != 0) return 0;
		const auto dot = name.rfind(".manifest");
		if (dot == std::string::npos || dot + 9 != name.size()) return 0;
		const auto gidStr = name.substr(prefix.size(), dot - prefix.size());
		if (gidStr.empty()) return 0;
		try { return std::stoull(gidStr); } catch (...) { return 0; }
	}

	// Copy `from`->`to` via a temp+rename so a reader never sees a short
	// file.  No-op (returns true) if `to` already exists non-empty.
	bool copyIfMissing(const fs::path& from, const fs::path& to)
	{
		std::error_code ec;
		if (fs::exists(to, ec) && fs::file_size(to, ec) > 0 && !ec) return true;

		fs::create_directories(to.parent_path(), ec);
		const fs::path tmp = to.string() + ".tmp";
		fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
		if (ec) { fs::remove(tmp, ec); return false; }
		fs::rename(tmp, to, ec);
		if (ec) { fs::remove(tmp, ec); return false; }
		return true;
	}
}

namespace ManifestStore
{
	std::string dir()
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};
		return std::string(home) + "/.config/SLSsteam/manifests";
	}

	void archiveDepot(uint32_t depotId)
	{
		const std::string root = steamRoot();
		const std::string store = dir();
		if (root.empty() || store.empty()) return;

		const fs::path dc = root + "/depotcache";
		const std::string prefix = std::to_string(depotId) + "_";

		std::error_code ec;
		if (!fs::is_directory(dc, ec)) return;

		for (const auto& entry : fs::directory_iterator(dc, ec))
		{
			if (ec) break;
			if (!entry.is_regular_file(ec)) continue;
			const auto name = entry.path().filename().string();
			if (!gidFromName(name, prefix)) continue;
			if (entry.file_size(ec) == 0 || ec) continue;

			if (copyIfMissing(entry.path(), fs::path(store) / name))
			{
				g_pLog->debug("ManifestStore: archived %s\n", name.c_str());
			}
		}
	}

	void archiveDepots(const std::vector<uint32_t>& depotIds)
	{
		for (uint32_t d : depotIds) archiveDepot(d);
	}

	bool restoreToDepotcache(uint32_t depotId, uint64_t gid)
	{
		if (!gid) return false;
		const std::string root = steamRoot();
		const std::string store = dir();
		if (root.empty() || store.empty()) return false;

		const std::string name = std::to_string(depotId) + "_"
		                         + std::to_string(gid) + ".manifest";
		const fs::path dest = fs::path(root) / "depotcache" / name;

		std::error_code ec;
		if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 0 && !ec)
		{
			return true; // already in depotcache
		}

		const fs::path src = fs::path(store) / name;
		if (!fs::exists(src, ec) || fs::file_size(src, ec) == 0) return false;

		fs::create_directories(dest.parent_path(), ec);
		if (copyIfMissing(src, dest))
		{
			g_pLog->info("ManifestStore: restored %s into depotcache\n",
			             name.c_str());
			return true;
		}
		return false;
	}

	uint64_t bestArchivedGid(uint32_t depotId, uint64_t excludeGid)
	{
		const std::string store = dir();
		if (store.empty()) return 0;

		std::error_code ec;
		if (!fs::is_directory(store, ec)) return 0;

		const std::string prefix = std::to_string(depotId) + "_";
		uint64_t best = 0;
		fs::file_time_type bestTime{};
		for (const auto& entry : fs::directory_iterator(store, ec))
		{
			if (ec) break;
			if (!entry.is_regular_file(ec)) continue;
			const auto name = entry.path().filename().string();
			const uint64_t gid = gidFromName(name, prefix);
			if (!gid || gid == excludeGid) continue;
			if (entry.file_size(ec) == 0 || ec) continue;

			const auto t = entry.last_write_time(ec);
			if (ec) continue;
			if (!best || t > bestTime) { best = gid; bestTime = t; }
		}
		return best;
	}

	void purgeDepots(const std::vector<uint32_t>& depotIds)
	{
		const std::string store = dir();
		if (store.empty()) return;

		std::error_code ec;
		if (!fs::is_directory(store, ec)) return;

		for (uint32_t depotId : depotIds)
		{
			const std::string prefix = std::to_string(depotId) + "_";
			for (const auto& entry : fs::directory_iterator(store, ec))
			{
				if (ec) break;
				const auto name = entry.path().filename().string();
				if (gidFromName(name, prefix))
				{
					fs::remove(entry.path(), ec);
					g_pLog->debug("ManifestStore: purged %s\n", name.c_str());
				}
			}
		}
	}
}
