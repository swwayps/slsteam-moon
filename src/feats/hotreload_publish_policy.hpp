// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "hotreload_types.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <vector>

namespace HotReloadPublishPolicy
{
inline constexpr bool shouldEvaluateInputs(
	bool initialPublication,
	bool membershipChanged,
	bool forceSourceRefresh) noexcept
{
	return initialPublication || membershipChanged || forceSourceRefresh;
}

inline constexpr bool shouldPublish(
	bool initialPublication,
	bool membershipChanged,
	bool fingerprintsChanged) noexcept
{
	return initialPublication || membershipChanged || fingerprintsChanged;
}

inline constexpr bool shouldAwaitDlcMetadata(
	bool initialPublication,
	bool addedBaseNeedsMetadata) noexcept
{
	return !initialPublication && addedBaseNeedsMetadata;
}

inline std::vector<std::uint32_t> membershipAppInfoRequestIds(
	bool initialPublication,
	const std::vector<std::uint32_t>& /*addedBaseIds*/,
	const std::vector<std::uint32_t>& cacheMissingBaseIds)
{
	// Runtime bases are prepared from the local normalized cache and reloaded
	// before ownership is published. Asking Steam for them here reopens the
	// synthetic clobber race and is redundant for ordinary appinfo too.
	std::vector<std::uint32_t> out = initialPublication
		? cacheMissingBaseIds : std::vector<std::uint32_t>{};
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	out.erase(std::remove(out.begin(), out.end(), 0), out.end());
	return out;
}

inline std::vector<std::uint32_t> readyBaseIds(
	const std::vector<std::uint32_t>& candidates,
	const std::unordered_set<std::uint32_t>& ready)
{
	std::vector<std::uint32_t> out;
	out.reserve(candidates.size());
	for (const std::uint32_t appId : candidates)
		if (appId != 0 && ready.count(appId) != 0) out.push_back(appId);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

inline std::unordered_set<std::uint32_t> guardedAppInfoIds(
	const std::unordered_set<std::uint32_t>& managed,
	const std::vector<std::uint32_t>& planner,
	const std::unordered_set<std::uint32_t>& authoritative)
{
	std::unordered_set<std::uint32_t> guarded(managed.begin(), managed.end());
	for (const std::uint32_t appId : planner)
		if (appId != 0) guarded.insert(appId);
	for (const std::uint32_t appId : authoritative)
		if (appId != 0) guarded.insert(appId);
	return guarded;
}

inline std::vector<std::uint32_t> nonAuthoritativeAppInfoRequestIds(
	std::vector<std::uint32_t> requested,
	const std::unordered_set<std::uint32_t>& authoritative)
{
	requested.erase(
		std::remove_if(requested.begin(), requested.end(),
			[&authoritative](std::uint32_t appId) {
				return appId == 0 || authoritative.count(appId) != 0;
			}),
		requested.end());
	std::sort(requested.begin(), requested.end());
	requested.erase(std::unique(requested.begin(), requested.end()), requested.end());
	return requested;
}

inline std::vector<std::uint32_t> newTopologyAppInfoRequestIds(
	const PackageSnapshot& previous,
	const PackageSnapshot& next)
{
	std::vector<std::uint32_t> previousIds = previous.appIds;
	std::vector<std::uint32_t> nextIds = next.appIds;
	std::sort(previousIds.begin(), previousIds.end());
	std::sort(nextIds.begin(), nextIds.end());
	previousIds.erase(
		std::unique(previousIds.begin(), previousIds.end()), previousIds.end());
	nextIds.erase(std::unique(nextIds.begin(), nextIds.end()), nextIds.end());

	std::vector<std::uint32_t> out;
	std::set_difference(
		nextIds.begin(), nextIds.end(), previousIds.begin(), previousIds.end(),
		std::back_inserter(out));
	out.erase(std::remove(out.begin(), out.end(), 0), out.end());
	return out;
}

inline constexpr bool metadataRepairDue(
	bool hasPending,
	std::uint64_t nowMs,
	std::uint64_t retryAfterMs) noexcept
{
	return hasPending && nowMs >= retryAfterMs;
}

inline constexpr std::uint64_t metadataRepairDeadlineMs(
	std::uint64_t nowMs,
	std::uint64_t delayMs) noexcept
{
	const auto max = std::numeric_limits<std::uint64_t>::max();
	return delayMs > max - nowMs ? max : nowMs + delayMs;
}

inline constexpr bool shouldArmMetadataRepair(
	bool postLoginOpportunitySeen) noexcept
{
	return !postLoginOpportunitySeen;
}

inline constexpr bool metadataCacheVisibleInSession(
	bool cacheReady,
	bool deferredUntilRestart) noexcept
{
	return cacheReady && !deferredUntilRestart;
}

inline constexpr bool metadataRepairDefersUntilRestart(
	bool runtimePending) noexcept
{
	return !runtimePending;
}

struct MetadataRepairCandidate
{
	std::uint32_t appId = 0;
	bool runtimePending = false;
	std::int64_t cacheMtimeSecs = 0;
};

inline std::vector<std::uint32_t> prioritizeMetadataRepairs(
	std::vector<MetadataRepairCandidate> candidates)
{
	std::sort(candidates.begin(), candidates.end(),
		[](const MetadataRepairCandidate& left,
		   const MetadataRepairCandidate& right) {
			if (left.runtimePending != right.runtimePending)
				return left.runtimePending > right.runtimePending;
			if (left.cacheMtimeSecs != right.cacheMtimeSecs)
				return left.cacheMtimeSecs > right.cacheMtimeSecs;
			return left.appId < right.appId;
		});
	std::vector<std::uint32_t> out;
	out.reserve(candidates.size());
	for (const auto& candidate : candidates)
		if (candidate.appId != 0) out.push_back(candidate.appId);
	return out;
}

inline std::vector<std::uint32_t> mergeMetadataRepairIds(
	const std::vector<std::uint32_t>& missing,
	const std::vector<std::uint32_t>& runtimePending)
{
	std::vector<std::uint32_t> out = missing;
	out.insert(out.end(), runtimePending.begin(), runtimePending.end());
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	out.erase(std::remove(out.begin(), out.end(), 0), out.end());
	return out;
}

inline std::uint32_t takeNextCacheRepairId(
	std::vector<std::uint32_t>& repairIds)
{
	if (repairIds.empty()) return 0;
	const std::uint32_t next = repairIds.front();
	std::rotate(repairIds.begin(), repairIds.begin() + 1, repairIds.end());
	return next;
}

inline bool metadataSnapshotChanged(
	const PackageSnapshot& previous,
	const PackageSnapshot& next) noexcept
{
	return previous.appIds != next.appIds ||
		previous.depotIds != next.depotIds ||
		previous.metadataComplete != next.metadataComplete;
}
} // namespace HotReloadPublishPolicy
