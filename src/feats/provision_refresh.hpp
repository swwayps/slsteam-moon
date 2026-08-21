// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure targeted refresh selection and single-worker request coalescing.

#pragma once

#include "provision_cache.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace AppInfoProvision
{

enum class RefreshReason : std::uint8_t
{
	PicsProductInfo = 1u << 0,
	PicsChanges     = 1u << 1,
	HotAdd          = 1u << 2,
	LocalInputs     = 1u << 3,
	CacheRepair     = 1u << 4,
	ForceFull       = 1u << 5,
	DlcMetadata     = 1u << 6,
};

constexpr std::uint8_t reasonMask(RefreshReason reason) noexcept
{
	return static_cast<std::uint8_t>(reason);
}

struct RefreshRequest
{
	std::uint32_t appId = 0;
	std::uint32_t minimumChangeNumber = 0;
	std::uint64_t managedGeneration = 0;
	std::uint8_t reasons = 0;
	bool forceRefresh = false;
	bool publishRuntime = false;
};

inline constexpr bool dlcMetadataPublishesLive(
	const RefreshRequest& request) noexcept
{
	const std::uint8_t metadata = reasonMask(RefreshReason::DlcMetadata);
	const std::uint8_t hotAdd = reasonMask(RefreshReason::HotAdd);
	const std::uint8_t cacheRepair = reasonMask(RefreshReason::CacheRepair);
	const bool metadataOnly = (request.reasons & ~metadata) == 0;
	const bool explicitlyHotAdded = (request.reasons & hotAdd) != 0;
	const bool startupCacheRecovery = (request.reasons & cacheRepair) != 0;
	return request.publishRuntime &&
		(metadataOnly || explicitlyHotAdded || startupCacheRecovery);
}

struct ObservedAppState
{
	std::uint32_t appId = 0;
	std::uint64_t managedGeneration = 0;
	std::uint32_t cachedChangeNumber = 0;
	std::uint32_t observedChangeNumber = 0;
	CacheReadiness readiness = CacheReadiness::Missing;
	bool terminalApplies = false;
	std::uint32_t terminalChangeNumber = 0;
};

struct RefreshPlan
{
	std::vector<RefreshRequest> requests;
	std::size_t ready = 0;
	std::size_t terminal = 0;
	std::size_t busy = 0;
};

enum class RefreshQueueAction { Ignore, Start, Queue };

struct RefreshQueueDecision
{
	RefreshQueueAction action = RefreshQueueAction::Ignore;
	std::vector<RefreshRequest> batch;
};

namespace detail
{

inline RefreshRequest mergeRefreshRequests(RefreshRequest current,
	                                          const RefreshRequest& incoming)
{
	if (incoming.managedGeneration > current.managedGeneration)
		return incoming;
	if (incoming.managedGeneration < current.managedGeneration)
		return current;

	if (incoming.minimumChangeNumber > current.minimumChangeNumber)
		current.minimumChangeNumber = incoming.minimumChangeNumber;
	current.reasons |= incoming.reasons;
	current.forceRefresh = current.forceRefresh || incoming.forceRefresh;
	current.publishRuntime = current.publishRuntime || incoming.publishRuntime;
	return current;
}

inline bool refreshRequestStrongerThan(const RefreshRequest& candidate,
	                                     const RefreshRequest& baseline)
{
	if (candidate.managedGeneration != baseline.managedGeneration)
		return candidate.managedGeneration > baseline.managedGeneration;
	return candidate.minimumChangeNumber > baseline.minimumChangeNumber ||
		(candidate.reasons & ~baseline.reasons) != 0 ||
		(candidate.forceRefresh && !baseline.forceRefresh) ||
		(candidate.publishRuntime && !baseline.publishRuntime);
}

inline bool sameRefreshRequest(const RefreshRequest& left,
	                            const RefreshRequest& right)
{
	return left.appId == right.appId &&
		left.minimumChangeNumber == right.minimumChangeNumber &&
		left.managedGeneration == right.managedGeneration &&
		left.reasons == right.reasons &&
		left.forceRefresh == right.forceRefresh &&
		left.publishRuntime == right.publishRuntime;
}

inline void mergeRefreshRequest(std::map<std::uint32_t, RefreshRequest>& requests,
	                               const RefreshRequest& incoming)
{
	if (incoming.appId == 0) return;
	const auto it = requests.find(incoming.appId);
	if (it == requests.end())
	{
		requests.emplace(incoming.appId, incoming);
		return;
	}
	it->second = mergeRefreshRequests(it->second, incoming);
}

inline std::vector<RefreshRequest> refreshRequestBatch(
	const std::map<std::uint32_t, RefreshRequest>& requests)
{
	std::vector<RefreshRequest> batch;
	batch.reserve(requests.size());
	for (const auto& [appId, request] : requests)
	{
		(void)appId;
		batch.push_back(request);
	}
	return batch;
}

} // namespace detail

inline RefreshPlan selectRefreshRequests(
	const std::vector<ObservedAppState>& states,
	RefreshReason reason)
{
	RefreshPlan plan;
	std::map<std::uint32_t, RefreshRequest> selected;
	for (const ObservedAppState& state : states)
	{
		if (state.appId == 0) continue;

		if (state.terminalApplies &&
			state.observedChangeNumber <= state.terminalChangeNumber)
		{
			++plan.terminal;
			continue;
		}

		const bool cacheReady = state.readiness == CacheReadiness::Fresh ||
			state.readiness == CacheReadiness::ValidStale;
		const bool hasNewerChange =
			state.observedChangeNumber > state.cachedChangeNumber;
		if (cacheReady && (state.observedChangeNumber == 0 || !hasNewerChange))
		{
			++plan.ready;
			continue;
		}

		if (state.readiness == CacheReadiness::Busy) ++plan.busy;
		detail::mergeRefreshRequest(selected, RefreshRequest{
			state.appId,
			state.observedChangeNumber,
			state.managedGeneration,
			reasonMask(reason),
			true,
			false,
		});
	}
	plan.requests = detail::refreshRequestBatch(selected);
	return plan;
}

class RefreshQueue
{
public:
	RefreshQueueDecision enqueue(const std::vector<RefreshRequest>& incoming)
	{
		if (!active_)
		{
			for (const RefreshRequest& request : incoming)
				detail::mergeRefreshRequest(pendingRequests_, request);
			if (pendingRequests_.empty()) return {};

			activeRequests_ = std::move(pendingRequests_);
			pendingRequests_.clear();
			active_ = true;
			return {RefreshQueueAction::Start,
			        detail::refreshRequestBatch(activeRequests_)};
		}

		bool queued = false;
		for (const RefreshRequest& request : incoming)
		{
			if (request.appId == 0) continue;
			const auto active = activeRequests_.find(request.appId);
			if (active == activeRequests_.end())
			{
				const auto pending = pendingRequests_.find(request.appId);
				const RefreshRequest before = pending == pendingRequests_.end()
					? RefreshRequest{} : pending->second;
				detail::mergeRefreshRequest(pendingRequests_, request);
				const RefreshRequest& after = pendingRequests_.at(request.appId);
				queued = queued || pending == pendingRequests_.end() ||
					!detail::sameRefreshRequest(before, after);
				continue;
			}

			const RefreshRequest merged =
				detail::mergeRefreshRequests(active->second, request);
			if (!detail::refreshRequestStrongerThan(merged, active->second))
				continue;
			const auto pending = pendingRequests_.find(request.appId);
			const RefreshRequest before = pending == pendingRequests_.end()
				? RefreshRequest{} : pending->second;
			detail::mergeRefreshRequest(pendingRequests_, merged);
			const RefreshRequest& after = pendingRequests_.at(request.appId);
			queued = queued || pending == pendingRequests_.end() ||
				!detail::sameRefreshRequest(before, after);
		}
		return {queued ? RefreshQueueAction::Queue : RefreshQueueAction::Ignore, {}};
	}

	std::vector<RefreshRequest> finishActive()
	{
		activeRequests_.clear();
		active_ = false;
		if (pendingRequests_.empty()) return {};

		activeRequests_ = std::move(pendingRequests_);
		pendingRequests_.clear();
		active_ = true;
		return detail::refreshRequestBatch(activeRequests_);
	}

	void restoreAfterStartFailure(const std::vector<RefreshRequest>& failed)
	{
		activeRequests_.clear();
		active_ = false;
		for (const RefreshRequest& request : failed)
			detail::mergeRefreshRequest(pendingRequests_, request);
	}

	bool active() const noexcept { return active_; }

private:
	bool active_ = false;
	std::map<std::uint32_t, RefreshRequest> activeRequests_;
	std::map<std::uint32_t, RefreshRequest> pendingRequests_;
};

} // namespace AppInfoProvision
