#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ManifestDonor
{
	struct PackageRecord
	{
		std::vector<uint32_t> apps;
		std::vector<uint32_t> depots;
	};

	struct LicensedPackages
	{
		std::unordered_set<uint32_t> ownedDepots;
		std::vector<std::pair<uint32_t, uint32_t>> licensedApps;
		std::size_t resolvedPackages = 0;
	};

	inline LicensedPackages deriveLicensedPackages(
	    const std::unordered_set<uint32_t>& licenses,
	    const std::unordered_map<uint32_t, PackageRecord>& packages)
	{
		LicensedPackages result;
		for (const uint32_t packageId : licenses)
		{
			const auto found = packages.find(packageId);
			if (found == packages.end()) continue;
			++result.resolvedPackages;
			result.ownedDepots.insert(
				found->second.depots.begin(), found->second.depots.end());
			for (const uint32_t appId : found->second.apps)
				if (appId) result.licensedApps.emplace_back(packageId, appId);
		}
		return result;
	}

	inline bool shouldObserveLicensedPackage(bool licenseReady,
	                                        bool packageInLicenseSet)
	{
		return licenseReady && packageInLicenseSet;
	}

	inline bool shouldSubmitCapturedCode(bool enabled,
	                                     bool currentGeneration,
	                                     bool licensedDepot,
	                                     bool wantedManifest,
	                                     bool fresh)
	{
		return enabled && currentGeneration && licensedDepot && wantedManifest
		       && fresh;
	}

	inline bool shouldRetainCapturedCode(bool currentGeneration,
	                                    bool wantedManifest,
	                                    bool ownershipComplete,
	                                    bool licensedDepot,
	                                    bool expired)
	{
		return currentGeneration && wantedManifest && !ownershipComplete
		       && !licensedDepot && !expired;
	}

	inline bool shouldFlushMintBatch(std::size_t pendingCodes,
	                                 std::int64_t ageMs) noexcept
	{
		return pendingCodes != 0 && ageMs >= 30000;
	}

	inline bool shouldForceWantedRefresh(std::int64_t nowMs,
	                                     std::int64_t lastAttemptMs,
	                                     std::int64_t refreshIntervalMs) noexcept
	{
		return lastAttemptMs == 0 ||
		       nowMs - lastAttemptMs >= refreshIntervalMs;
	}

	inline bool isBaseUrlAllowed(std::string_view url)
	{
		if (url.starts_with("https://")) return url.size() > 8;
		for (const std::string_view prefix : {
			"http://127.0.0.1", "http://localhost", "http://[::1]"
		})
		{
			if (!url.starts_with(prefix)) continue;
			if (url.size() == prefix.size()) return true;
			const char boundary = url[prefix.size()];
			if (boundary == ':' || boundary == '/') return true;
		}
		return false;
	}

	struct Wanted
	{
		uint32_t appId = 0;
		uint32_t depotId = 0;
		uint64_t gid = 0;
	};

	inline bool parseWantedLine(std::string_view line, Wanted& out)
	{
		const auto first = line.find(':');
		if (first == line.npos) return false;
		const auto second = line.find(':', first + 1);
		if (second == line.npos || line.find(':', second + 1) != line.npos)
			return false;
		auto parse = [](std::string_view s, uint64_t& value)
		{
			if (s.empty()) return false;
			const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
			return result.ec == std::errc{} && result.ptr == s.data() + s.size();
		};
		uint64_t app = 0, depot = 0, gid = 0;
		if (!parse(line.substr(0, first), app) ||
		    !parse(line.substr(first + 1, second - first - 1), depot) ||
		    !parse(line.substr(second + 1), gid) ||
		    app > UINT32_MAX || depot == 0 || depot > UINT32_MAX || gid == 0)
			return false;
		out = {static_cast<uint32_t>(app), static_cast<uint32_t>(depot), gid};
		return true;
	}
}
