// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure scheduling policy for Phase 4.5 provisioning.

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

// A known startup failure leaves no worker to drain a queued request, so the
// request must remain retryable. An uncertain detach recovery may still have
// a live worker; that worker must retain the gate and drain the queue itself.
inline bool shouldRequeueRefreshAfterStartFailure(bool workerMayStillExist)
{
	return !workerMayStillExist;
}

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

// PICS provisioning can be triggered for a game added after setup() as well
// as for apps already managed at boot. Warm the loader unconditionally so a
// later hot-add cannot become the first worker-thread dlopen.
inline bool shouldWarmCurlBeforePics(bool /*hasManagedApps*/)
{
	return true;
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

// A cache accepted only as an explicit stale fallback must remain eligible for
// a newer raw PICS response. Fresh/provider-normalized cold results can be
// marked in-process so the callback does not race their publication.
inline bool shouldMarkColdCacheSanitized(bool explicitFallback)
{
	return !explicitFallback;
}

} // namespace AppInfoProvision
