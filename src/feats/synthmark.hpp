// SPDX-License-Identifier: AGPL-3.0-only
//
// synthmark — persistent "synthetic appinfo" markers + outgoing-PICS strip.
//
// Why this exists
// ---------------
// Token-locked titles (their PICS product-info access token is DENIED to
// the client — e.g. Risk of Rain 2, app 632360) come back from Steam's
// runtime RequestAppInfoUpdate with an EMPTY product-info buffer.  When
// that empty refresh lands, Steam overwrites the depots + installdir that
// manifestsynth rebuilt into appinfo.vdf at startup, so the install dialog
// drops to "0 B" and fails with "Invalid install path".
//
// The cure is to keep Steam from ever re-fetching those apps: provisioning
// MARKS an app synthetic when it had to rebuild depots from local
// manifests, and the outgoing-PICS hook (apps.cpp::sendPICSInfoRequest)
// STRIPS marked apps from Steam's product-info request.  Steam then never
// receives the empty refresh and keeps the appinfo we spliced at startup.
//
// The mark must be PERSISTED, not just in-memory: Steam re-execs setup()
// several times per boot, and the surviving process can hit the
// provisioning cache (TTL) and skip synthesis entirely — an in-memory set
// would be empty in exactly the process that issues the PICS requests.  A
// marker file (`<cacheDir>/synthetic_<appid>`) survives that.
//
// Kept dependency-light (only std + std::filesystem, dir injected) so it is
// host-unit-testable (tools/test_synthmark.cpp); the real cache dir is
// supplied by appinfo_provision.cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <filesystem>

namespace SynthMark
{
	inline std::string markerPath(const std::string& dir, uint32_t appId)
	{
		return dir + "/synthetic_" + std::to_string(appId);
	}

	// Record that appId's appinfo depots were synthesized (token-locked).
	// Idempotent; returns true if the marker exists afterwards.
	inline bool mark(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const auto path = markerPath(dir, appId);
		if (std::filesystem::exists(path, ec)) return true;
		std::FILE* f = std::fopen(path.c_str(), "w");
		if (!f) return false;
		std::fclose(f);
		return true;
	}

	// Remove a marker (remove-game cleanup).  Returns true if it is gone
	// afterwards (including when it never existed).
	inline bool unmark(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		std::filesystem::remove(markerPath(dir, appId), ec);
		return !std::filesystem::exists(markerPath(dir, appId), ec);
	}

	// True iff appId is marked synthetic.  A single stat; cheap enough to
	// call per outgoing PICS request.  Missing dir / file -> false.
	inline bool isMarked(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		return std::filesystem::exists(markerPath(dir, appId), ec);
	}

	// Every marked appid in `dir` (empty if the dir is missing).
	inline std::unordered_set<uint32_t> loadAll(const std::string& dir)
	{
		std::unordered_set<uint32_t> out;
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec)) return out;
		for (const auto& e : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			const auto name = e.path().filename().string();
			constexpr const char* kPrefix = "synthetic_";
			if (name.rfind(kPrefix, 0) != 0) continue;
			const std::string idStr = name.substr(std::char_traits<char>::length(kPrefix));
			if (idStr.empty()) continue;
			try
			{
				size_t consumed = 0;
				const unsigned long v = std::stoul(idStr, &consumed);
				if (consumed == idStr.size() && v != 0)
					out.insert(static_cast<uint32_t>(v));
			}
			catch (...) { continue; }
		}
		return out;
	}

	// Indices (DESCENDING) of `requestedAppIds` that are synthetic, so the
	// caller can delete them in place from a protobuf repeated field without
	// invalidating the not-yet-processed indices.  Pure.
	inline std::vector<int> stripIndices(
	    const std::vector<uint32_t>& requestedAppIds,
	    const std::function<bool(uint32_t)>& isSynthetic)
	{
		std::vector<int> out;
		for (int i = static_cast<int>(requestedAppIds.size()) - 1; i >= 0; --i)
			if (isSynthetic(requestedAppIds[static_cast<size_t>(i)]))
				out.push_back(i);
		return out;
	}
}
