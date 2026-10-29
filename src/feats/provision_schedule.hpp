// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure scheduling policy for appinfo provisioning: which refreshed apps are
// published at runtime, and whether preinit provisions every managed app or
// only those without a usable cache before the appinfo splice.

#pragma once

#include "provision_cache.hpp"
#include "provision_refresh.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

inline std::vector<std::uint32_t> selectRuntimePublishCandidates(
	const std::vector<RefreshRequest>& requested,
	const std::unordered_set<std::uint32_t>& managed,
	const std::unordered_set<std::uint32_t>& synthetic,
	const std::unordered_map<std::uint32_t, std::uint64_t>& generations)
{
	std::vector<std::uint32_t> selected;
	selected.reserve(requested.size());
	for (const RefreshRequest& request : requested)
	{
		if (request.appId == 0 || !request.publishRuntime ||
			managed.count(request.appId) == 0)
			continue;
		const auto generation = generations.find(request.appId);
		if (generation == generations.end() ||
			generation->second != request.managedGeneration)
			continue;
		const bool localUpdate =
			(request.reasons & (reasonMask(RefreshReason::HotAdd) |
			                    reasonMask(RefreshReason::LocalInputs))) != 0;
		const bool suppressedSynthetic =
			synthetic.count(request.appId) != 0 &&
			(request.reasons & (reasonMask(RefreshReason::PicsProductInfo) |
			                    reasonMask(RefreshReason::PicsChanges) |
			                    reasonMask(RefreshReason::ForceFull))) != 0;
		if (localUpdate || suppressedSynthetic)
			selected.push_back(request.appId);
	}
	std::sort(selected.begin(), selected.end());
	selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
	return selected;
}

enum class PreinitProvisionAction
{
	SynchronousProvision,
	ColdFallbackThenSplice,
};

inline PreinitProvisionAction preinitProvisionAction(bool asyncEnabled)
{
	return asyncEnabled
		? PreinitProvisionAction::ColdFallbackThenSplice
		: PreinitProvisionAction::SynchronousProvision;
}

// Resolve the config value and the optional field override. Invalid explicit
// values fail closed: a field/debug typo must not silently enable a new
// startup behavior in the field.
inline bool asyncProvisionEnabled(bool configEnabled, const char* envOverride)
{
	if (!envOverride || *envOverride == '\0') return configEnabled;

	const std::string_view value(envOverride);
	if (value == "1" || value == "true" || value == "yes" || value == "on")
		return true;
	if (value == "0" || value == "false" || value == "no" || value == "off")
		return false;
	return false;
}


} // namespace AppInfoProvision
