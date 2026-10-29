#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ManifestIndex
{
	struct ManifestName
	{
		uint32_t depotId = 0;
		uint64_t gid = 0;
	};

	inline std::optional<ManifestName> parseManifestName(std::string_view name)
	{
		constexpr std::string_view suffix = ".manifest";
		if (name.size() <= suffix.size()
		    || name.substr(name.size() - suffix.size()) != suffix)
			return std::nullopt;

		const auto stem = name.substr(0, name.size() - suffix.size());
		const auto separator = stem.find('_');
		if (separator == std::string_view::npos
		    || stem.find('_', separator + 1) != std::string_view::npos)
			return std::nullopt;

		const auto depotText = stem.substr(0, separator);
		const auto gidText = stem.substr(separator + 1);
		if (depotText.empty() || gidText.empty()) return std::nullopt;

		uint32_t depotId = 0;
		uint64_t gid = 0;
		const auto depotResult = std::from_chars(
			depotText.data(), depotText.data() + depotText.size(), depotId, 10);
		const auto gidResult = std::from_chars(
			gidText.data(), gidText.data() + gidText.size(), gid, 10);
		if (depotResult.ec != std::errc{}
		    || depotResult.ptr != depotText.data() + depotText.size()
		    || gidResult.ec != std::errc{}
		    || gidResult.ptr != gidText.data() + gidText.size()
		    || depotId == 0 || gid == 0)
			return std::nullopt;

		return ManifestName{depotId, gid};
	}

	// Calls visit(name, mtime) for every regular `<depot>_<gid>.manifest` in
	// `dir`. Steam's download hooks reach this, so a read error ends the walk
	// instead of throwing, and an entry that cannot be read skips only itself.
	template <typename Visit>
	inline void forEachManifest(const std::string& dir, Visit visit)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::directory_iterator end;
		for (fs::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec))
		{
			std::error_code entryError;
			if (!it->is_regular_file(entryError) || entryError) continue;
			const auto parsed = parseManifestName(it->path().filename().string());
			if (!parsed) continue;
			const auto mtime = it->last_write_time(entryError);
			if (entryError) continue;
			visit(*parsed, mtime);
		}
	}
}
