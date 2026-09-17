// SPDX-License-Identifier: AGPL-3.0-only
//
// Filesystem-only primitives for ManifestStore. Kept independent from Steam,
// logging, and global configuration so atomic publication is unit-testable.

#pragma once

#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unistd.h>

namespace ManifestStoreIO
{
	namespace fs = std::filesystem;
	using ArchivedGidIndex = std::unordered_map<std::uint32_t, std::uint64_t>;

	inline bool parseManifestName(std::string_view name,
	                              std::uint32_t& depotId,
	                              std::uint64_t& gid) noexcept
	{
		constexpr std::string_view suffix = ".manifest";
		if (!name.ends_with(suffix)) return false;
		name.remove_suffix(suffix.size());
		const std::size_t separator = name.find('_');
		if (separator == std::string_view::npos || separator == 0 ||
			separator + 1 >= name.size() ||
			name.find('_', separator + 1) != std::string_view::npos)
		{
			return false;
		}

		std::uint64_t parsedDepot = 0;
		const std::string_view depotText = name.substr(0, separator);
		const auto depotResult = std::from_chars(
			depotText.data(), depotText.data() + depotText.size(), parsedDepot);
		if (depotResult.ec != std::errc{} ||
			depotResult.ptr != depotText.data() + depotText.size() ||
			parsedDepot == 0 ||
			parsedDepot > std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}

		const std::string_view gidText = name.substr(separator + 1);
		const auto gidResult = std::from_chars(
			gidText.data(), gidText.data() + gidText.size(), gid);
		if (gidResult.ec != std::errc{} ||
			gidResult.ptr != gidText.data() + gidText.size() || gid == 0)
		{
			return false;
		}
		depotId = static_cast<std::uint32_t>(parsedDepot);
		return true;
	}

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

	inline ArchivedGidIndex newestValidManifestGids(const fs::path& storeDir)
	{
		struct Observation
		{
			std::uint64_t gid = 0;
			fs::file_time_type mtime{};
		};
		std::unordered_map<std::uint32_t, Observation> observations;
		std::error_code ec;
		if (!fs::is_directory(storeDir, ec) || ec) return {};

		fs::directory_iterator it(storeDir, ec);
		const fs::directory_iterator end;
		while (!ec && it != end)
		{
			const fs::path path = it->path();
			std::uint32_t depotId = 0;
			std::uint64_t gid = 0;
			const std::string name = path.filename().string();
			if (parseManifestName(name, depotId, gid) &&
				isValidManifest(path))
			{
				std::error_code timeError;
				const fs::file_time_type mtime = fs::last_write_time(path, timeError);
				if (!timeError)
				{
					auto found = observations.find(depotId);
					if (found == observations.end() ||
						mtime > found->second.mtime)
					{
						observations[depotId] = {gid, mtime};
					}
				}
			}
			it.increment(ec);
		}

		ArchivedGidIndex result;
		result.reserve(observations.size());
		for (const auto& [depotId, observation] : observations)
			result.emplace(depotId, observation.gid);
		return result;
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

	struct PublishResult
	{
		bool staged = false;
		bool archived = false;
	};

	inline PublishResult publishBestEffort(const fs::path& source,
	                                      const fs::path& stored,
	                                      const fs::path& staged)
	{
		if (!isValidManifest(source)) return {};

		PublishResult result;
		result.archived = atomicCopy(source, stored) && isValidManifest(stored);
		if (result.archived)
		{
			result.staged = restore(stored, staged);
			return result;
		}

		// The durable copy is an extra resilience layer. If only that location
		// is unavailable, still publish the validated bytes atomically where
		// Steam expects them so the current install can continue.
		std::error_code ec;
		fs::remove(staged, ec);
		result.staged = atomicCopy(source, staged) && isValidManifest(staged);
		return result;
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
