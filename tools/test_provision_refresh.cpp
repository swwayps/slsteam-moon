// TDD regression tests for targeted AppInfo refresh selection and coalescing.

#include "feats/provision_refresh.hpp"
#include "feats/appinfo_provision.hpp"

#include <cstdio>
#include <unordered_set>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(condition, message)                                           \
	do                                                                      \
	{                                                                       \
		if (!(condition))                                                    \
		{                                                                   \
			std::printf("FAIL: %s\n", message);                             \
			++failures;                                                       \
		}                                                                   \
		else                                                                \
		{                                                                   \
			std::printf("ok:   %s\n", message);                             \
		}                                                                   \
	} while (0)
}

int main()
{
	using namespace AppInfoProvision;

	const std::vector<std::pair<std::uint32_t, CacheReadiness>> currentResponse{
		{4496490, CacheReadiness::Missing},
	};
	CHECK(selectColdStartApps(currentResponse, ColdStartMode::RuntimeMissingOnly) ==
	          std::unordered_set<std::uint32_t>{4496490},
	      "runtime cold recovery contains only the current response app");

	const std::vector<std::pair<std::uint32_t, CacheReadiness>> contention{
		{420530, CacheReadiness::Busy},
		{4496490, CacheReadiness::Unverified},
	};
	CHECK(selectColdStartApps(contention, ColdStartMode::RuntimeMissingOnly).empty(),
	      "busy and unverified pairs never enter synchronous callback recovery");

	const std::vector<std::pair<std::uint32_t, CacheReadiness>> startupCache{
		{420530, CacheReadiness::ValidStale},
		{4496490, CacheReadiness::Missing},
		{1621690, CacheReadiness::Invalid},
	};
	CHECK((selectColdStartApps(startupCache, ColdStartMode::StartupRequireUsablePair) ==
	          std::unordered_set<std::uint32_t>{4496490, 1621690}),
	      "startup recovery retains only missing and invalid cache pairs");
	CHECK(coldStartPrimesTerminalMemo(
	          ColdStartMode::StartupRequireUsablePair) &&
	      !coldStartPrimesTerminalMemo(ColdStartMode::RuntimeMissingOnly),
	      "startup primes disk terminal state while runtime remains memo-only");

	const std::vector<ObservedAppState> fleet{
		{420530, 3, 10, 7, CacheReadiness::ValidStale, false, 0},
		{4496490, 3, 11, 9, CacheReadiness::Missing, true, 9},
	};
	CHECK(selectRefreshRequests(fleet, RefreshReason::PicsProductInfo).requests.empty(),
	      "unchanged stale cache and matching terminal state select no work");

	const std::vector<ObservedAppState> changed{
		{420530, 3, 10, 11, CacheReadiness::Fresh, false, 0},
	};
	const auto changedPlan =
		selectRefreshRequests(changed, RefreshReason::PicsChanges);
	CHECK(changedPlan.requests.size() == 1 &&
		  changedPlan.requests.front().appId == 420530 &&
		  changedPlan.requests.front().minimumChangeNumber == 11,
	      "newer PICS change selects exactly one managed app");
	CHECK(!changedPlan.requests.front().publishRuntime,
	      "normal PICS change remains disk-only until Steam delivery is suppressed");

	const std::vector<ObservedAppState> busy{
		{420530, 3, 10, 10, CacheReadiness::Busy, false, 0},
	};
	const auto busyPlan =
		selectRefreshRequests(busy, RefreshReason::CacheRepair);
	CHECK(busyPlan.requests.size() == 1 &&
		  busyPlan.requests.front().forceRefresh,
	      "busy cache defers one targeted repair without becoming a cold miss");

	const RefreshRequest sameChange{
		420530, 7, 2, reasonMask(RefreshReason::PicsProductInfo), false, false};
	CHECK(!requestNeedsFetch(sameChange, CacheReadiness::ValidStale, 7),
	      "TTL expiry alone does not force a runtime fetch");

	const RefreshRequest localChange{
		420530, 0, 2, reasonMask(RefreshReason::LocalInputs), true, false};
	CHECK(requestNeedsFetch(localChange, CacheReadiness::Fresh, 7),
	      "changed local inputs re-render even a fresh cache pair");

	const RefreshRequest metadataOnly{
		420530, 0, 2, reasonMask(RefreshReason::DlcMetadata), true, false};
	CHECK(!requestNeedsFetch(metadataOnly, CacheReadiness::Fresh, 7) &&
	      !requestNeedsFetch(metadataOnly, CacheReadiness::Missing, 0),
	      "DLC metadata repair never refetches or rewrites the base app cache");
	CHECK(!dlcMetadataPublishesLive(metadataOnly),
	      "startup migration persists metadata without touching live Steam state");
	RefreshRequest pendingMetadataRepair = metadataOnly;
	pendingMetadataRepair.publishRuntime = true;
	CHECK(dlcMetadataPublishesLive(pendingMetadataRepair),
	      "a failed live hot-add completion remains live on retry");
	RefreshRequest metadataAndHotAdd = metadataOnly;
	metadataAndHotAdd.reasons |= reasonMask(RefreshReason::HotAdd);
	CHECK(requestNeedsFetch(metadataAndHotAdd, CacheReadiness::Fresh, 7),
	      "metadata repair coalescing never suppresses a real hot-add refresh");
	CHECK(!dlcMetadataPublishesLive(metadataAndHotAdd),
	      "a hot-add reason without explicit runtime authorization stays disk-only");
	metadataAndHotAdd.publishRuntime = true;
	CHECK(dlcMetadataPublishesLive(metadataAndHotAdd),
	      "an explicitly authorized hot-add publishes its child metadata live");
	RefreshRequest startupCacheRepair = metadataOnly;
	startupCacheRepair.reasons = reasonMask(RefreshReason::CacheRepair);
	startupCacheRepair.publishRuntime = true;
	CHECK(dlcMetadataPublishesLive(startupCacheRepair),
	      "an authorized startup cache recovery publishes child metadata live");
	RefreshRequest coalescedLegacy = metadataOnly;
	coalescedLegacy.reasons |= reasonMask(RefreshReason::LocalInputs);
	coalescedLegacy.publishRuntime = true;
	CHECK(!dlcMetadataPublishesLive(coalescedLegacy),
	      "unrelated runtime refresh cannot promote legacy metadata migration");

	const RefreshRequest newerChange{
		420530, 8, 2, reasonMask(RefreshReason::PicsChanges), true, false};
	CHECK(requestNeedsFetch(newerChange, CacheReadiness::Fresh, 7),
	      "newer PICS change forces a targeted fetch");

	RefreshQueue queue;
	const RefreshRequest first{
		420530, 8, 3, reasonMask(RefreshReason::PicsChanges), true, false};
	const auto firstDecision = queue.enqueue({first});
	CHECK(firstDecision.action == RefreshQueueAction::Start &&
		  firstDecision.batch.size() == 1,
	      "first non-empty request starts one worker batch");

	const auto duplicateDecision = queue.enqueue({first});
	CHECK(duplicateDecision.action == RefreshQueueAction::Ignore,
	      "an in-flight request dominates an identical callback");

	const RefreshRequest newer{
		420530, 9, 3, reasonMask(RefreshReason::PicsChanges), true, false};
	const auto newerDecision = queue.enqueue({newer});
	CHECK(newerDecision.action == RefreshQueueAction::Queue,
	      "a newer change for an in-flight app is queued once");

	const auto nextBatch = queue.finishActive();
	CHECK(nextBatch.size() == 1 && nextBatch.front().minimumChangeNumber == 9,
	      "finishing the active batch drains only the newer pending request");

	const RefreshRequest suppressedSynthetic{
		420530, 9, 3, reasonMask(RefreshReason::PicsChanges), true, true};
	const auto publicationDecision = queue.enqueue({suppressedSynthetic});
	CHECK(publicationDecision.action == RefreshQueueAction::Queue,
	      "suppressed synthetic runtime publication upgrades an active request");
	const auto publicationBatch = queue.finishActive();
	CHECK(publicationBatch.size() == 1 && publicationBatch.front().publishRuntime,
	      "request coalescing preserves suppressed synthetic runtime publication");

	RefreshQueue generationQueue;
	const RefreshRequest generationThree{
		420530, 8, 3, reasonMask(RefreshReason::PicsChanges), true, false};
	const RefreshRequest generationFour{
		420530, 1, 4, reasonMask(RefreshReason::HotAdd), true, false};
	generationQueue.enqueue({generationThree});
	generationQueue.enqueue({generationFour});
	CHECK(generationQueue.enqueue({newer}).action == RefreshQueueAction::Ignore,
	      "a newer pending generation dominates a stale active-generation callback");

	CHECK(queue.enqueue({}).action == RefreshQueueAction::Ignore,
	      "an empty refresh request never starts a thread");

	if (failures != 0)
	{
		std::printf("test_provision_refresh: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("provision refresh tests passed");
	return 0;
}
