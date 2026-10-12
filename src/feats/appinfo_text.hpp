// SPDX-License-Identifier: AGPL-3.0-only
//
// Reads the depots block of a provisioned appinfo text buffer
// (`picsbuffer_<appid>.bin`).
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace AppInfoText
{

using DepotGid = std::pair<uint32_t, uint64_t>;

// (depotId, manifests.public.gid) of every depot that lists a public gid, in
// buffer order and regardless of OS or DLC status. Depots without one
// (config-only or DLC stubs) contribute nothing.
inline std::vector<DepotGid> extractDepotsAndGids(const std::string& buf)
{
	std::vector<DepotGid> depots;
	std::size_t depotsPos = buf.find("\"depots\"");
	if (depotsPos == std::string::npos) return {};

	std::size_t openBrace = buf.find('{', depotsPos + 8);
	if (openBrace == std::string::npos) return {};

	int depth = 1;
	std::size_t scan = openBrace + 1;
	DepotGid* current = nullptr;
	std::string currentSection;
	std::string currentBranch;

	auto entryFor = [&depots](uint32_t depotId) -> DepotGid* {
		for (auto& e : depots)
		{
			if (e.first == depotId) return &e;
		}
		depots.push_back({depotId, 0});
		return &depots.back();
	};
	auto isDigits = [](const std::string& token) {
		if (token.empty()) return false;
		for (char ch : token)
		{
			if (ch < '0' || ch > '9') return false;
		}
		return true;
	};
	// `token` is all digits here, so this only fails when it does not fit.
	auto parse = [](const std::string& token, auto& value) {
		return std::from_chars(token.data(), token.data() + token.size(), value).ec ==
		       std::errc();
	};

	while (scan < buf.size() && depth > 0)
	{
		char c = buf[scan];
		if (c == '"')
		{
			std::size_t end = buf.find('"', scan + 1);
			if (end == std::string::npos) break;
			std::string token = buf.substr(scan + 1, end - scan - 1);
			scan = end + 1;

			if (depth == 1)
			{
				uint32_t depotId = 0;
				if (isDigits(token))
					current = parse(token, depotId) ? entryFor(depotId) : nullptr;
			}
			else if (depth == 2)
			{
				currentSection = token;
			}
			else if (depth == 3 && currentSection == "manifests")
			{
				currentBranch = token;
			}
			else if (depth == 4 && currentSection == "manifests" &&
			         currentBranch == "public" && token == "gid" && current)
			{
				std::size_t valStart = buf.find('"', scan);
				if (valStart != std::string::npos)
				{
					std::size_t valEnd = buf.find('"', valStart + 1);
					if (valEnd != std::string::npos)
					{
						const std::string value =
						    buf.substr(valStart + 1, valEnd - valStart - 1);
						uint64_t gid = 0;
						if (isDigits(value) && parse(value, gid)) current->second = gid;
						scan = valEnd + 1;
					}
				}
			}
			continue;
		}
		if (c == '{') ++depth;
		else if (c == '}')
		{
			--depth;
			if (depth == 1) current = nullptr;
		}
		++scan;
	}

	std::erase_if(depots, [](const DepotGid& d) { return d.second == 0; });
	return depots;
}

} // namespace AppInfoText
