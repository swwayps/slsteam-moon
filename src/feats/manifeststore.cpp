// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifeststore.hpp for design notes.

#include "manifeststore.hpp"
#include "manifeststore_io.hpp"
#include "manifestsynth.hpp"

#include "../globals.hpp"
#include "../log.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sys/stat.h>
#include <system_error>
#include <unordered_map>

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

	std::string manifestName(uint32_t depotId, uint64_t gid)
	{
		return std::to_string(depotId) + "_" + std::to_string(gid)
		    + ".manifest";
	}

	struct ManifestKey
	{
		uint32_t depotId;
		uint64_t gid;

		bool operator==(const ManifestKey&) const = default;
	};

	struct ManifestKeyHash
	{
		size_t operator()(const ManifestKey& key) const
		{
			return std::hash<uint64_t>{}(
			    (static_cast<uint64_t>(key.depotId) << 32) ^ key.gid);
		}
	};

	std::mutex g_sizeMutex;
	std::unordered_map<ManifestKey, uint64_t, ManifestKeyHash> g_installedSizes;

	std::optional<uint64_t> parseInstalledSize(const fs::path& path)
	{
		if (!ManifestStoreIO::isValidManifest(path)) return std::nullopt;
		std::ifstream input(path, std::ios::binary);
		if (!input) return std::nullopt;
		const std::string bytes{std::istreambuf_iterator<char>(input),
		                        std::istreambuf_iterator<char>()};
		uint64_t installed = 0;
		uint64_t download = 0;
		if (!ManifestSynth::parseManifestSizes(bytes, installed, download)
		    || installed == 0)
			return std::nullopt;
		return installed;
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
			if (!ManifestStoreIO::isValidManifest(entry.path())) continue;

			const fs::path archived = fs::path(store) / name;
			if (ManifestStoreIO::isValidManifest(archived)
			    || ManifestStoreIO::atomicCopy(entry.path(), archived))
			{
				g_pLog->debug("ManifestStore: archived %s\n", name.c_str());
			}
		}
	}

	void archiveDepots(const std::vector<uint32_t>& depotIds)
	{
		for (uint32_t d : depotIds) archiveDepot(d);
	}

	bool archiveManifest(uint32_t depotId, uint64_t gid)
	{
		if (!depotId || !gid) return false;
		const std::string root = steamRoot();
		const std::string store = dir();
		if (root.empty() || store.empty()) return false;

		const std::string name = manifestName(depotId, gid);
		const fs::path source = fs::path(root) / "depotcache" / name;
		const fs::path target = fs::path(store) / name;
		if (!ManifestStoreIO::isValidManifest(source)) return false;
		if (ManifestStoreIO::isValidManifest(target)) return true;
		if (!ManifestStoreIO::atomicCopy(source, target)) return false;

		g_pLog->debug("ManifestStore: archived exact %s\n", name.c_str());
		return true;
	}

	bool publishDownloadedManifest(uint32_t depotId, uint64_t gid,
	                               const std::string& sourcePath)
	{
		if (!depotId || !gid || sourcePath.empty()) return false;
		const std::string root = steamRoot();
		const std::string store = dir();
		if (root.empty() || store.empty()) return false;

		const std::string name = manifestName(depotId, gid);
		const fs::path archived = fs::path(store) / name;
		const fs::path staged = fs::path(root) / "depotcache" / name;
		if (!ManifestStoreIO::publish(sourcePath, archived, staged)) return false;

		g_pLog->info("ManifestStore: published downloaded %s (store -> depotcache)\n",
		             name.c_str());
		return true;
	}

	bool isInDepotcache(uint32_t depotId, uint64_t gid)
	{
		if (!gid) return false;
		const std::string root = steamRoot();
		if (root.empty()) return false;

		const std::string name = manifestName(depotId, gid);
		const fs::path path = fs::path(root) / "depotcache" / name;
		return ManifestStoreIO::isValidManifest(path);
	}

	bool isArchived(uint32_t depotId, uint64_t gid)
	{
		if (!depotId || !gid) return false;
		const std::string store = dir();
		if (store.empty()) return false;
		return ManifestStoreIO::isValidManifest(
		    fs::path(store) / manifestName(depotId, gid));
	}

	void cacheInstalledSize(uint32_t depotId, uint64_t gid, uint64_t size)
	{
		if (!depotId || !gid || !size) return;
		std::lock_guard lock(g_sizeMutex);
		g_installedSizes[{depotId, gid}] = size;
	}

	std::optional<uint64_t> installedSize(uint32_t depotId, uint64_t gid)
	{
		if (!depotId || !gid) return std::nullopt;
		const ManifestKey key{depotId, gid};
		{
			std::lock_guard lock(g_sizeMutex);
			if (const auto it = g_installedSizes.find(key);
			    it != g_installedSizes.end())
				return it->second;
		}

		std::optional<uint64_t> size;
		const std::string store = dir();
		if (!store.empty())
			size = parseInstalledSize(fs::path(store) / manifestName(depotId, gid));
		if (!size)
		{
			const std::string root = steamRoot();
			if (!root.empty())
				size = parseInstalledSize(
				    fs::path(root) / "depotcache" / manifestName(depotId, gid));
		}
		if (size) cacheInstalledSize(depotId, gid, *size);
		return size;
	}

	bool restoreToDepotcache(uint32_t depotId, uint64_t gid)
	{
		if (!gid) return false;
		const std::string root = steamRoot();
		const std::string store = dir();
		if (root.empty() || store.empty()) return false;

		const std::string name = manifestName(depotId, gid);
		const fs::path dest = fs::path(root) / "depotcache" / name;
		const fs::path src = fs::path(store) / name;
		if (ManifestStoreIO::restore(src, dest))
		{
			g_pLog->info("ManifestStore: restored %s into depotcache\n",
			             name.c_str());
			return true;
		}
		return false;
	}

	bool markPreferredGid(uint32_t depotId, uint64_t gid)
	{
		const std::string store = dir();
		if (store.empty() || !isArchived(depotId, gid)) return false;
		return ManifestStoreIO::writePreferred(store, depotId, gid);
	}

	uint64_t preferredArchivedGid(uint32_t depotId, uint64_t excludeGid)
	{
		const std::string store = dir();
		if (store.empty()) return 0;
		const uint64_t gid =
		    ManifestStoreIO::readPreferred(store, depotId, excludeGid);
		return isArchived(depotId, gid) ? gid : 0;
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
			if (!ManifestStoreIO::isValidManifest(entry.path())) continue;

			const auto t = entry.last_write_time(ec);
			if (ec) continue;
			if (!best || t > bestTime) { best = gid; bestTime = t; }
		}
		return best;
	}

	ArchivedGidIndex archivedGidIndex()
	{
		const std::string store = dir();
		if (store.empty()) return {};
		return ManifestStoreIO::newestValidManifestGids(store);
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
			fs::remove(ManifestStoreIO::preferredPath(store, depotId), ec);
		}
	}
}
