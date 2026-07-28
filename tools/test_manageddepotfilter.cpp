// Standalone regression test for manageddepotfilter.hpp.
//
// Build (from repo root):
//   g++ -std=c++20 tools/test_manageddepotfilter.cpp -o /tmp/test_manageddepotfilter
//   /tmp/test_manageddepotfilter

#include "../src/feats/manageddepotfilter.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

struct DepotEntry
{
	uint32_t depotId;
	uint32_t reserved04;
	uint64_t manifestGid;
	uint64_t size;
	uint32_t dlcAppId;
	uint32_t reserved1c;
};

static_assert(sizeof(DepotEntry) == ManagedDepotFilter::kDepotEntryStride);

int main()
{
	// Mirrors Vampire Survivors: a real base depot, one empty/content pair
	// for a DLC, an unrelated empty depot, and another managed empty/content
	// pair.  Only the managed size-zero placeholders belong outside both the
	// install plan and the appinfo-derived reconciliation target.
	DepotEntry entries[] = {
		{1794681, 11, 504073999575923137ULL, 1165700087, 0,       12},
		{2230761, 21, 7674937276999197104ULL, 0,          2230760, 22},
		{2230763, 31, 8197371098964655276ULL, 15302817,   2230760, 32},
		{4000001, 41, 111,                    0,          4000000, 42},
		{2313551, 51, 6298930804943469542ULL, 0,          2313550, 52},
		{2313553, 61, 1834948645539404906ULL, 22739509,   2313550, 62},
	};
	const std::unordered_set<uint32_t> managed = {
		1794681, 2230761, 2230763, 2313551, 2313553,
	};
	std::vector<uint32_t> dropped;

	const int32_t kept = ManagedDepotFilter::compactEmptyManaged(
		entries, 6,
		[&](uint32_t depotId) { return managed.count(depotId) != 0; },
		[&](uint32_t depotId) { dropped.push_back(depotId); });

	CHECK(kept == 4, "two managed size-zero depots are removed");
	CHECK(dropped.size() == 2 && dropped[0] == 2230761 &&
	          dropped[1] == 2313551,
	      "removed depot ids are reported in source order");
	CHECK(entries[0].depotId == 1794681 &&
	          entries[1].depotId == 2230763 &&
	          entries[2].depotId == 4000001 &&
	          entries[3].depotId == 2313553,
	      "surviving depots retain their relative order");
	CHECK(entries[1].manifestGid == 8197371098964655276ULL &&
	          entries[1].size == 15302817 &&
	          entries[1].dlcAppId == 2230760 &&
	          entries[1].reserved04 == 31 && entries[1].reserved1c == 32,
	      "compaction preserves every field in a moved entry");
	CHECK(entries[2].depotId == 4000001 && entries[2].size == 0,
	      "an unmanaged size-zero depot is not filtered");

	DepotEntry allEmpty[] = {
		{5000001, 0, 101, 0, 5000000, 0},
		{5000002, 0, 102, 0, 5000000, 0},
	};
	const int32_t noneKept = ManagedDepotFilter::compactEmptyManaged(
		allEmpty, 2,
		[](uint32_t) { return true; },
		[](uint32_t) {});
	CHECK(noneKept == 0, "a vector containing only managed empty depots becomes empty");

	CHECK(ManagedDepotFilter::compactEmptyManaged(
	          nullptr, 3, [](uint32_t) { return true; }, [](uint32_t) {}) == 3,
	      "a null vector is left unchanged defensively");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
