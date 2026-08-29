// SPDX-License-Identifier: AGPL-3.0-only
//
// Bounded, deterministic inputs for live package-state reconciliation.
#pragma once

#include "hotreload_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HotReloadInputs
{

inline constexpr std::size_t kMaxSnapshotIds = 4096;

struct AppInput
{
	std::uint32_t baseAppId = 0;
	bool cacheValid = false;
	std::vector<std::uint32_t> plannerAppIds;
	std::vector<std::uint32_t> depotIds;
	bool childMetadataPending = false;
	bool childMetadataMissing = false;
	std::int64_t cacheMtimeSecs = 0;
	bool publishReady = true;
};

struct BuildResult
{
	PackageSnapshot snapshot;
	std::vector<std::uint32_t> cacheMissingBaseIds;
	std::vector<std::uint32_t> metadataMissingBaseIds;
	std::unordered_map<std::uint32_t, std::int64_t> cacheMtimeSecs;
	bool valid = false;
};

inline BuildResult build(std::uint64_t generation,
	const std::vector<AppInput>& inputs,
	std::size_t maxIds = kMaxSnapshotIds)
{
	BuildResult out;
	out.snapshot.generation = generation;
	out.snapshot.metadataComplete = true;

	std::unordered_set<std::uint32_t> appIds;
	std::unordered_set<std::uint32_t> depotIds;
	appIds.reserve(std::min(inputs.size(), maxIds));
	depotIds.reserve(std::min(inputs.size(), maxIds));

	for (const AppInput& input : inputs)
	{
		if (input.publishReady && input.baseAppId != 0)
			appIds.insert(input.baseAppId);
		else if (input.publishReady)
			out.snapshot.metadataComplete = false;

		if (input.cacheValid)
		{
			if (input.publishReady)
				for (const std::uint32_t appId : input.plannerAppIds)
					if (appId != 0) appIds.insert(appId);
		}
		else
		{
			if (input.publishReady)
				out.snapshot.metadataComplete = false;
			if (input.baseAppId != 0)
				out.cacheMissingBaseIds.push_back(input.baseAppId);
		}

		if (input.publishReady)
			for (const std::uint32_t depotId : input.depotIds)
				if (depotId != 0) depotIds.insert(depotId);
		if (input.publishReady && input.childMetadataPending)
			out.snapshot.metadataComplete = false;
		if (input.childMetadataMissing && input.baseAppId != 0)
			out.metadataMissingBaseIds.push_back(input.baseAppId);
		if (input.baseAppId != 0)
			out.cacheMtimeSecs[input.baseAppId] = input.cacheMtimeSecs;

		if (appIds.size() > maxIds || depotIds.size() > maxIds)
		{
			out.snapshot.appIds.clear();
			out.snapshot.depotIds.clear();
			out.snapshot.metadataComplete = false;
			return out;
		}
	}

	out.snapshot.appIds.assign(appIds.begin(), appIds.end());
	out.snapshot.depotIds.assign(depotIds.begin(), depotIds.end());
	std::sort(out.snapshot.appIds.begin(), out.snapshot.appIds.end());
	std::sort(out.snapshot.depotIds.begin(), out.snapshot.depotIds.end());
	std::sort(out.cacheMissingBaseIds.begin(),
	          out.cacheMissingBaseIds.end());
	std::sort(out.metadataMissingBaseIds.begin(),
	          out.metadataMissingBaseIds.end());
	out.valid = true;
	return out;
}

BuildResult buildFromCaches(std::uint64_t generation,
	const std::unordered_set<std::uint32_t>& managedAppIds,
	const std::unordered_set<std::uint32_t>& metadataPendingBaseIds,
	const std::unordered_set<std::uint32_t>& metadataDeferredBaseIds,
	const std::unordered_set<std::uint32_t>& readyBaseIds);

} // namespace HotReloadInputs
