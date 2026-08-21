// Standalone tests for package contribution provenance and reconciliation.
//
// Build (from repo root):
//   g++ -std=c++20 -Wall -Wextra -Wpedantic
//   tools/test_hotreload_package.cpp -o /tmp/test_hotreload_package

#include "../src/feats/hotreload_package.hpp"
#include "../src/feats/hotreload_types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using IdSet = std::unordered_set<uint32_t>;

static IdSet setOf(std::initializer_list<uint32_t> ids)
{
	return IdSet(ids);
}

static std::vector<uint32_t> asVector(const uint32_t* data, uint32_t size)
{
	if (!data || size == 0) return {};
	return std::vector<uint32_t>(data, data + size);
}

int main()
{
	using HotReloadPackage::AppInfoRequestState;
	using HotReloadPackage::Contribution;
	using HotReloadPackage::aggregate;
	using HotReloadPackage::compactInjected;
	using HotReloadPackage::carryPendingSnapshotWork;
	using HotReloadPackage::idsToRequestAfterApply;
	using HotReloadPackage::missingFromVector;
	using HotReloadPackage::snapshotAppInfoRequestIdsAfterApply;

	// Shared app and depot identifiers remain desired while at least one
	// active base contributes them; inactive bases contribute nothing.
	{
		const std::vector<Contribution> contributions{
			{10, {10, 1000}, {110, 999}},
			{20, {20, 1000}, {220, 999}},
		};
		const auto both = aggregate({10, 20}, contributions);
		CHECK(both.appIds == setOf({10, 20, 1000}),
		      "aggregate: shared app ids deduplicate");
		CHECK(both.depotIds == setOf({110, 220, 999}),
		      "aggregate: shared depot ids deduplicate");

		const auto one = aggregate({20}, contributions);
		CHECK(one.appIds == setOf({20, 1000}),
		      "aggregate: inactive base app ids do not leak");
		CHECK(one.depotIds == setOf({220, 999}),
		      "aggregate: shared depot remains while referenced");
	}

	// Only obsolete entries with explicit plugin provenance are removed.
	// The write pass is stable and keeps the shared id 1000.
	{
		uint32_t data[] = {7, 10, 1000, 20, 999};
		uint32_t size = 5;
		compactInjected(data, size, setOf({10, 1000, 20, 999}),
		                setOf({20, 1000}));
		CHECK(asVector(data, size) == std::vector<uint32_t>({7, 1000, 20}),
		      "compact: removes only obsolete seeded entries in stable order");
	}

	// An id present naturally in the live vector is not reported missing, so
	// a caller that appends only the result cannot claim it as plugin-seeded.
	{
		uint32_t data[] = {50, 50, 70};
		const auto missing =
			missingFromVector(data, 3, setOf({50, 60, 70, 80}));
		CHECK(missing == std::set<uint32_t>({60, 80}),
		      "missing: naturally present ids are not plugin-owned");
	}

	// Duplicate live values are handled by value provenance: obsolete seeded
	// duplicates are both removed, while duplicate non-seeded values survive.
	{
		uint32_t seededData[] = {4, 99, 99, 8};
		uint32_t seededSize = 4;
		compactInjected(seededData, seededSize, setOf({99}), setOf({}));
		CHECK(asVector(seededData, seededSize) == std::vector<uint32_t>({4, 8}),
		      "compact: duplicate obsolete seeded values are removed");

		uint32_t naturalData[] = {4, 4, 99};
		uint32_t naturalSize = 3;
		compactInjected(naturalData, naturalSize, setOf({99}), setOf({}));
		CHECK(asVector(naturalData, naturalSize) == std::vector<uint32_t>({4, 4}),
		      "compact: duplicate non-seeded values retain order");

		uint32_t desiredData[] = {99, 99};
		uint32_t desiredSize = 2;
		compactInjected(desiredData, desiredSize, setOf({99}), setOf({99}));
		CHECK(asVector(desiredData, desiredSize) ==
		          std::vector<uint32_t>({99, 99}),
		      "compact: duplicate desired values are retained");
	}

	// Non-seeded ids are preserved whether or not they are desired, and a
	// seeded id is preserved once it is still desired.
	{
		uint32_t data[] = {3, 10, 20, 30};
		uint32_t size = 4;
		compactInjected(data, size, setOf({10, 30}), setOf({30}));
		CHECK(asVector(data, size) == std::vector<uint32_t>({3, 20, 30}),
		      "compact: non-seeded desired and undesired ids survive");
	}

	// A zero-length vector is a no-op for compaction and reports every
	// requested id as missing without dereferencing its data pointer.
	{
		uint32_t size = 0;
		compactInjected(nullptr, size, setOf({1, 2}), setOf({}));
		CHECK(size == 0, "compact: zero-sized vector stays zero-sized");
		CHECK(missingFromVector(nullptr, 0, setOf({9, 3, 7})) ==
		          std::set<uint32_t>({3, 7, 9}),
		      "missing: zero-sized vector returns sorted desired ids");
	}

	// Removing every seeded entry updates the caller-owned size to zero.
	{
		uint32_t data[] = {11, 12, 13};
		uint32_t size = 3;
		compactInjected(data, size, setOf({11, 12, 13}), setOf({}));
		CHECK(size == 0 && asVector(data, size).empty(),
		      "compact: all obsolete injected entries are removed");
	}

	// Missing results are deterministic and deduplicated even when desired is
	// supplied as an ordered sequence with repeats.
	{
		uint32_t data[] = {7};
		const std::vector<uint32_t> desired = {9, 3, 9, 7, 5};
		CHECK(missingFromVector(data, 1, desired) ==
		          std::set<uint32_t>({3, 5, 9}),
		      "missing: absent ids are deduplicated and sorted");
	}

	// A runtime appinfo request is derived from exactly the ids inserted by a
	// successful package-vector transaction. Failed/partial applies request
	// nothing, while naturally present ids never enter the request.
	{
		uint32_t data[] = {7, 3405340};
		const auto missing = missingFromVector(
			data, 2, setOf({7, 1149460, 3405340}));
		CHECK(idsToRequestAfterApply(false, missing).empty(),
		      "appinfo request: failed package apply requests nothing");
		CHECK(idsToRequestAfterApply(true, missing) ==
		          std::vector<uint32_t>({1149460}),
		      "appinfo request: successful apply requests only the new id");
		const std::vector<uint32_t> generationAdditions{1149460, 3405340};
		CHECK(idsToRequestAfterApply(true, generationAdditions) ==
		          generationAdditions,
		      "appinfo request: generation additions survive an already-present package id");
		PackageSnapshot snapshot;
		snapshot.addedAppIds = {668580};
		snapshot.appInfoRequestIds = {2214820, 2214821};
		CHECK(snapshotAppInfoRequestIdsAfterApply(false, snapshot).empty(),
		      "appinfo request: failed snapshot apply requests nothing");
		CHECK(snapshotAppInfoRequestIdsAfterApply(true, snapshot) ==
		          std::vector<uint32_t>({2214820, 2214821}),
		      "appinfo request: snapshot uses child refresh ids, not base UI additions");

		PackageSnapshot pendingBase;
		pendingBase.generation = 2;
		pendingBase.appIds = {668580};
		pendingBase.addedAppIds = {668580};
		pendingBase.appInfoRequestIds = {668580};
		PackageSnapshot metadataCompletion;
		metadataCompletion.generation = 3;
		metadataCompletion.appIds = {668580, 2214820, 2214821};
		metadataCompletion.appInfoRequestIds = {2214820, 2214821};
		const auto deferred = carryPendingSnapshotWork(
			pendingBase, metadataCompletion,
			/*previousOwnershipProcessed=*/false,
			/*previousAppInfoRequested=*/false);
		CHECK(deferred.addedAppIds == std::vector<uint32_t>({668580}) &&
		      deferred.appInfoRequestIds ==
		          std::vector<uint32_t>({668580, 2214820, 2214821}),
		      "deferred apply carries base ownership work into DLC completion");
		const auto licenseDeferred = carryPendingSnapshotWork(
			pendingBase, metadataCompletion,
			/*previousOwnershipProcessed=*/false,
			/*previousAppInfoRequested=*/true);
		CHECK(licenseDeferred.addedAppIds ==
		          std::vector<uint32_t>({668580}) &&
		      licenseDeferred.appInfoRequestIds ==
		          std::vector<uint32_t>({2214820, 2214821}),
		      "package apply does not retire base UI work before license processing");
		CHECK(carryPendingSnapshotWork(
			pendingBase, metadataCompletion,
			/*previousOwnershipProcessed=*/true,
			/*previousAppInfoRequested=*/true) ==
		          metadataCompletion,
		      "an applied base generation does not leak old one-shot work forward");
		const auto requestRace = carryPendingSnapshotWork(
			pendingBase, metadataCompletion,
			/*previousOwnershipProcessed=*/true,
			/*previousAppInfoRequested=*/false);
		CHECK(requestRace.addedAppIds.empty() &&
		      requestRace.appInfoRequestIds ==
		          std::vector<uint32_t>({668580, 2214820, 2214821}),
		      "DLC completion carries a base request not yet accepted by Steam");

		AppInfoRequestState requestState;
		const std::vector<uint32_t> childRequests{2214820, 2214821};
		CHECK(requestState.reserve(3, childRequests) == childRequests,
		      "request reservation claims every unrequested child");
		CHECK(requestState.reserve(3, childRequests).empty(),
		      "a concurrent caller cannot claim in-flight child requests");
		requestState.finish(3, childRequests, /*accepted=*/false);
		CHECK(requestState.reserve(3, childRequests) == childRequests,
		      "a rejected request releases its reservation for retry");
		requestState.finish(3, childRequests, /*accepted=*/true);
		CHECK(requestState.reserve(3, childRequests).empty() &&
		      requestState.allAccepted(3, childRequests),
		      "accepted child requests remain deduplicated after completion");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
