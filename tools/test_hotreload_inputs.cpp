#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload_inputs.hpp"
#include "../src/feats/hotreload_publish_policy.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
}

int main()
{
	using HotReloadInputs::AppInput;

	const std::vector<AppInput> inputs{
		{20, false, {}, {220}},
		{10, true, {1000, 10, 1000}, {999, 110, 999}},
	};
	const auto built = HotReloadInputs::build(7, inputs);
	check(built.valid, "bounded fixture builds successfully");
	check(built.snapshot.generation == 7, "generation is preserved");
	check(built.snapshot.appIds ==
		std::vector<std::uint32_t>({10, 20, 1000}),
		"base and planner app ids are deterministic");
	check(built.snapshot.depotIds ==
		std::vector<std::uint32_t>({110, 220, 999}),
		"depot ids are deterministic");
	check(!built.snapshot.metadataComplete,
		"missing or malformed cache is unresolved");
	check(built.cacheMissingBaseIds ==
		std::vector<std::uint32_t>({20}),
		"an invalid base cache is retained for post-login recovery");
	check(HotReloadPublishPolicy::membershipAppInfoRequestIds(
		/*initialPublication=*/true, {10, 20}, built.cacheMissingBaseIds) ==
		std::vector<std::uint32_t>({20}),
		"cold publication requests only bases whose cache is unusable");
	check(HotReloadPublishPolicy::membershipAppInfoRequestIds(
		/*initialPublication=*/false, {30}, built.cacheMissingBaseIds) ==
		std::vector<std::uint32_t>{},
		"runtime addition never requests network appinfo before local readiness");
	check(HotReloadPublishPolicy::readyBaseIds(
		{420530, 582010}, {582010}) == std::vector<std::uint32_t>({582010}),
		"only prepared additions become generation-visible ownership work");
	check(HotReloadPublishPolicy::guardedAppInfoIds(
			/*managed=*/{100}, /*planner=*/{100, 200},
			/*authoritative=*/{300}) ==
			std::unordered_set<std::uint32_t>({100, 200, 300}),
		"marker-only compatibility authority enters the appinfo guard scope");
	check(HotReloadPublishPolicy::guardedAppInfoIds(
			/*managed=*/{100}, /*planner=*/{100}, /*authoritative=*/{}) ==
			std::unordered_set<std::uint32_t>({100}),
		"removed compatibility authority cannot survive the next scope publication");
	check(HotReloadPublishPolicy::nonAuthoritativeAppInfoRequestIds(
			{300, 100, 200, 200, 0}, {200, 300}) ==
			std::vector<std::uint32_t>({100}),
		"locally loaded appinfo is not immediately requested back from Steam");
	std::ifstream hotReloadSource("src/feats/hotreload.cpp");
	const std::string hotReloadText(
		(std::istreambuf_iterator<char>(hotReloadSource)),
		std::istreambuf_iterator<char>());
	check(hotReloadText.find("nonAuthoritativeAppInfoRequestIds(") !=
		std::string::npos,
		"hot reload filters local authority before package refresh requests");

	const auto empty = HotReloadInputs::build(8, {});
	check(empty.valid && empty.snapshot.metadataComplete &&
		empty.snapshot.appIds.empty() && empty.snapshot.depotIds.empty(),
		"an empty managed source is a complete empty snapshot");

	AppInput hiddenPending;
	hiddenPending.baseAppId = 420530;
	hiddenPending.cacheValid = true;
	hiddenPending.plannerAppIds = {420530};
	hiddenPending.depotIds = {420531, 420534};
	hiddenPending.publishReady = false;
	const auto hidden = HotReloadInputs::build(81, {hiddenPending});
	check(hidden.valid && hidden.snapshot.metadataComplete &&
		hidden.snapshot.appIds.empty() && hidden.snapshot.depotIds.empty(),
		"a desired but unready base contributes no package or depot state");
	check(hidden.cacheMissingBaseIds.empty(),
		"a hidden validated cache is not misclassified as missing");
	hiddenPending.cacheValid = false;
	const auto hiddenMissing = HotReloadInputs::build(82, {hiddenPending});
	check(hiddenMissing.valid && hiddenMissing.snapshot.metadataComplete &&
		hiddenMissing.snapshot.appIds.empty() &&
		hiddenMissing.snapshot.depotIds.empty(),
		"a hidden missing cache does not make the visible package incomplete");
	check(hiddenMissing.cacheMissingBaseIds ==
		std::vector<std::uint32_t>({420530}),
		"a hidden missing cache remains scheduled for preparation");

	const auto bounded = HotReloadInputs::build(9, inputs, 2);
	check(!bounded.valid && !bounded.snapshot.metadataComplete &&
		bounded.snapshot.appIds.empty() && bounded.snapshot.depotIds.empty(),
		"oversized input fails closed without a destructive partial snapshot");
	const std::vector<AppInput> pendingInputs{
		{1245620, true, {2778580}, {1245621}, true, true, 300},
	};
	const auto pending = HotReloadInputs::build(10, pendingInputs);
	check(pending.valid && !pending.snapshot.metadataComplete &&
		pending.snapshot.appIds ==
			std::vector<std::uint32_t>({1245620, 2778580}) &&
		pending.metadataMissingBaseIds ==
			std::vector<std::uint32_t>({1245620}) &&
		pending.cacheMtimeSecs.at(1245620) == 300,
		"pending child metadata keeps the first runtime topology unresolved");

	check(HotReloadPublishPolicy::shouldEvaluateInputs(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*forceSourceRefresh=*/true),
		"a forced source event evaluates local inputs");
	check(!HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/false),
		"an unchanged duplicate forced event does not publish");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/true, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/false),
		"the initial state always publishes");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/true,
		/*fingerprintsChanged=*/false),
		"a membership transition always publishes");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/true),
		"a key or archived fallback change publishes");
	check(!HotReloadPublishPolicy::shouldEvaluateInputs(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*forceSourceRefresh=*/false),
		"an ordinary unchanged event skips fingerprint evaluation");
	check(HotReloadPublishPolicy::shouldAwaitDlcMetadata(
		/*initialPublication=*/false, /*addedBaseNeedsMetadata=*/true),
		"a runtime addition stays pending until its child metadata is live");
	check(!HotReloadPublishPolicy::shouldAwaitDlcMetadata(
		/*initialPublication=*/true, /*addedBaseNeedsMetadata=*/true),
		"cold boot retains the established package publication behavior");
	check(!HotReloadPublishPolicy::shouldAwaitDlcMetadata(
		/*initialPublication=*/false, /*addedBaseNeedsMetadata=*/false),
		"a base without DLC candidates is immediately complete");
	check(HotReloadPublishPolicy::metadataRepairDue(
		/*hasPending=*/true, /*nowMs=*/100, /*retryAfterMs=*/100),
		"pending metadata repair runs when its cooldown expires");
	check(!HotReloadPublishPolicy::metadataRepairDue(
		/*hasPending=*/true, /*nowMs=*/99, /*retryAfterMs=*/100),
		"pending metadata repair respects its cooldown");
	check(!HotReloadPublishPolicy::metadataRepairDue(
		/*hasPending=*/false, /*nowMs=*/100, /*retryAfterMs=*/0),
		"empty metadata repair state does no work");
	check(HotReloadPublishPolicy::metadataCacheVisibleInSession(
		/*cacheReady=*/true, /*deferredUntilRestart=*/false) &&
		!HotReloadPublishPolicy::metadataCacheVisibleInSession(
			/*cacheReady=*/true, /*deferredUntilRestart=*/true) &&
		!HotReloadPublishPolicy::metadataCacheVisibleInSession(
			/*cacheReady=*/false, /*deferredUntilRestart=*/false),
		"disk-only migration cannot leak into a later live package generation");
	check(HotReloadPublishPolicy::metadataRepairDefersUntilRestart(
			/*runtimePending=*/false) &&
		!HotReloadPublishPolicy::metadataRepairDefersUntilRestart(
			/*runtimePending=*/true),
		"legacy repair is deferred before enqueue while hot-add stays live");
	const auto startupRepairAfter =
		HotReloadPublishPolicy::metadataRepairDeadlineMs(
			/*nowMs=*/500, /*delayMs=*/30000);
	check(HotReloadPublishPolicy::shouldArmMetadataRepair(
			/*postLoginOpportunitySeen=*/false) &&
		!HotReloadPublishPolicy::shouldArmMetadataRepair(
			/*postLoginOpportunitySeen=*/true) &&
		startupRepairAfter == 30500 &&
		!HotReloadPublishPolicy::metadataRepairDue(
			/*hasPending=*/true, /*nowMs=*/30499, startupRepairAfter) &&
		HotReloadPublishPolicy::metadataRepairDue(
			/*hasPending=*/true, /*nowMs=*/30500, startupRepairAfter),
		"startup grace keeps legacy repair out of the login and splash window");
	const auto prioritizedRepairs =
		HotReloadPublishPolicy::prioritizeMetadataRepairs({
			{10, false, 300},
			{20, true, 100},
			{30, false, 500},
			{40, true, 50},
		});
	check(prioritizedRepairs ==
		std::vector<std::uint32_t>({20, 40, 30, 10}),
		"hot-add pending bases precede newest-first migration repairs");
	check(HotReloadPublishPolicy::mergeMetadataRepairIds(
		{10, 30}, {20, 30, 0}) ==
		std::vector<std::uint32_t>({10, 20, 30}),
		"failed hot-add completion joins migration repair without duplicates");
	std::vector<std::uint32_t> cacheRepairs{10, 20, 30};
	check(HotReloadPublishPolicy::takeNextCacheRepairId(cacheRepairs) == 10 &&
		HotReloadPublishPolicy::takeNextCacheRepairId(cacheRepairs) == 20 &&
		cacheRepairs == std::vector<std::uint32_t>({30, 10, 20}),
		"failed cold repairs rotate so every missing base receives a turn");

	PackageSnapshot previous;
	previous.generation = 4;
	previous.appIds = {1245620};
	previous.depotIds = {1245621};
	previous.metadataComplete = false;
	PackageSnapshot completed = previous;
	completed.generation = 5;
	completed.appIds = {1245620, 2778580};
	completed.metadataComplete = true;
	check(HotReloadPublishPolicy::metadataSnapshotChanged(previous, completed),
		"metadata completion publishes the newly discovered DLC topology");
	check(HotReloadPublishPolicy::newTopologyAppInfoRequestIds(
		previous, completed) == std::vector<std::uint32_t>({2778580}),
		"metadata completion requests appinfo for the newly introduced child");
	PackageSnapshot duplicate = completed;
	duplicate.generation = 99;
	check(!HotReloadPublishPolicy::metadataSnapshotChanged(completed, duplicate),
		"generation alone does not republish an identical metadata snapshot");

	const std::vector<ProvisionTerminal::LocalInput> localInputs{
		{330, "", 999},
		{110, "alpha", 999},
		{220, "beta", 999},
	};
	const ManifestStore::ArchivedGidIndex archivedGids{
		{110, 19},
		{220, 3},
	};
	check(AppInfoProvision::fingerprintIndexedLocalInputs(
		localInputs, archivedGids) == "baba8bfd866131f5",
		"indexed gids preserve terminal fingerprint semantics");

	return failures == 0 ? 0 : 1;
}
