// SPDX-License-Identifier: AGPL-3.0-only
//
// Size-based rotation of the session log, done once when Steam starts.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace LogRotate
{
	inline constexpr std::uintmax_t kMaxBytes = 8u << 20;
	inline constexpr int kKeep = 3;

	// Once `path` holds at least `maxBytes`, it becomes path.1 and the older
	// path.N shift up; only `keep` of them survive. Never throws.
	inline void rotate(const std::string& path, std::uintmax_t maxBytes = kMaxBytes,
	                   int keep = kKeep)
	{
		std::error_code ec;
		const std::uintmax_t size = std::filesystem::file_size(path, ec);
		if (ec || size < maxBytes || keep < 1)
			return;
		for (int i = keep - 1; i >= 1; --i)
			std::filesystem::rename(path + "." + std::to_string(i),
			                        path + "." + std::to_string(i + 1), ec);
		std::filesystem::rename(path, path + ".1", ec);
	}
}
