// SPDX-License-Identifier: AGPL-3.0-only
//
// Filesystem-only primitives for ManifestStore. Kept independent from Steam,
// logging, and global configuration so atomic publication is unit-testable.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace ManifestStoreIO
{
	namespace fs = std::filesystem;

	inline fs::path uniqueTempPath(const fs::path& target)
	{
		static std::atomic<uint64_t> counter{0};
		return target.string() + ".slsteam_tmp."
		    + std::to_string(static_cast<unsigned long>(getpid())) + "."
		    + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
	}

	inline bool isValidManifest(const fs::path& path)
	{
		std::error_code ec;
		if (!fs::is_regular_file(path, ec) || ec) return false;
		if (fs::file_size(path, ec) < sizeof(uint32_t) || ec) return false;

		std::ifstream in(path, std::ios::binary);
		uint32_t magic = 0;
		if (!in.read(reinterpret_cast<char*>(&magic), sizeof(magic))) return false;
		return magic == 0x71F617D0u;
	}

	inline bool atomicCopy(const fs::path& source, const fs::path& target)
	{
		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		if (ec) return false;

		const fs::path tmp = uniqueTempPath(target);
		fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
		if (ec)
		{
			fs::remove(tmp, ec);
			return false;
		}

		fs::rename(tmp, target, ec);
		if (ec)
		{
			fs::remove(tmp, ec);
			return false;
		}
		return true;
	}

	inline bool restore(const fs::path& stored, const fs::path& staged)
	{
		if (!isValidManifest(stored)) return false;
		if (isValidManifest(staged)) return true;

		std::error_code ec;
		fs::remove(staged, ec);
		if (!atomicCopy(stored, staged)) return false;
		return isValidManifest(staged);
	}

	inline bool publish(const fs::path& source, const fs::path& stored,
	                    const fs::path& staged)
	{
		if (!isValidManifest(source)) return false;

		// Store first: once this succeeds, a concurrent Steam depotcache purge
		// cannot destroy the only copy of a freshly downloaded manifest.
		if (!atomicCopy(source, stored) || !isValidManifest(stored)) return false;
		return restore(stored, staged);
	}

	inline fs::path preferredPath(const fs::path& storeDir, uint32_t depotId)
	{
		return storeDir / (".preferred_" + std::to_string(depotId));
	}

	inline bool writePreferred(const fs::path& storeDir, uint32_t depotId,
	                           uint64_t gid)
	{
		if (!depotId || !gid) return false;

		std::error_code ec;
		fs::create_directories(storeDir, ec);
		if (ec) return false;

		const fs::path target = preferredPath(storeDir, depotId);
		const fs::path tmp = uniqueTempPath(target);
		{
			std::ofstream out(tmp, std::ios::trunc);
			if (!out.is_open()) return false;
			out << gid << '\n';
			if (!out.good())
			{
				out.close();
				fs::remove(tmp, ec);
				return false;
			}
		}

		fs::rename(tmp, target, ec);
		if (ec)
		{
			fs::remove(tmp, ec);
			return false;
		}
		return true;
	}

	inline uint64_t readPreferred(const fs::path& storeDir, uint32_t depotId,
	                              uint64_t excludeGid)
	{
		if (!depotId) return 0;
		std::ifstream in(preferredPath(storeDir, depotId));
		std::string text;
		if (!std::getline(in, text) || text.empty()) return 0;

		uint64_t gid = 0;
		try
		{
			std::size_t used = 0;
			gid = std::stoull(text, &used);
			if (used != text.size()) return 0;
		}
		catch (...)
		{
			return 0;
		}
		return gid && gid != excludeGid ? gid : 0;
	}
}
