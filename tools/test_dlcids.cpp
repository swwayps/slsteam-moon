// Standalone test for AppInfoProvision::extractDlcAppIds.
//
// Root cause (proven on the Zorin VM 2026-06-05 with Binding of Isaac
// 250900): Steam's install planner only schedules a `dlcappid`-tagged
// depot when the DLC's appid is present in package 0's AppIdVec.  The
// existing PackagePatch injected only AdditionalApps (base ids) into
// AppIdVec, so DLC appids — which live in the base app's appinfo as
// `extended/listofdlc` and `depots/<id>/dlcappid` — were never injected
// and their depots were filtered out of the install.
//
// extractDlcAppIds(wire) parses a provisioned appinfo wire-text VDF
// buffer (the `picsbuffer_<appid>.bin` format) and returns every DLC
// appid it advertises, from BOTH sources, deduplicated, excluding the
// base appid itself.  This is the set we must add to AppIdVec.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_dlcids.cpp -o /tmp/test_dlcids && /tmp/test_dlcids

#include "../src/feats/dlcids.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool has(const std::vector<uint32_t>& v, uint32_t x)
{
	return std::find(v.begin(), v.end(), x) != v.end();
}

int main()
{
	// 1) listofdlc only: comma-separated list under extended.
	{
		const std::string wire =
			"\"appinfo\"\n{\n"
			"\t\"appid\"\t\t\"250900\"\n"
			"\t\"extended\"\n\t{\n"
			"\t\t\"listofdlc\"\t\t\"401920,570660,1426300,3353470\"\n"
			"\t}\n"
			"}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 250900);
		CHECK(dlc.size() == 4, "listofdlc: parses all 4 ids");
		CHECK(has(dlc, 401920) && has(dlc, 570660) &&
		      has(dlc, 1426300) && has(dlc, 3353470),
		      "listofdlc: ids correct");
	}

	// 2) dlcappid only: scattered in depot entries, with duplicates.
	{
		const std::string wire =
			"\"appinfo\"\n{\n"
			"\t\"depots\"\n\t{\n"
			"\t\t\"250905\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"401920\"\n\t\t}\n"
			"\t\t\"250906\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"401920\"\n\t\t}\n"
			"\t\t\"250911\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"1426300\"\n\t\t}\n"
			"\t}\n"
			"}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 250900);
		CHECK(dlc.size() == 2, "dlcappid: dedups repeats (401920 x2 -> 1)");
		CHECK(has(dlc, 401920) && has(dlc, 1426300), "dlcappid: ids correct");
	}

	// 3) both sources merge and dedup against each other.
	{
		const std::string wire =
			"\"appinfo\"\n{\n"
			"\t\"extended\"\n\t{\n"
			"\t\t\"listofdlc\"\t\t\"401920,570660\"\n"
			"\t}\n"
			"\t\"depots\"\n\t{\n"
			"\t\t\"250911\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"1426300\"\n\t\t}\n"
			"\t\t\"250906\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"401920\"\n\t\t}\n"
			"\t}\n"
			"}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 250900);
		CHECK(dlc.size() == 3, "merge: 401920,570660 + 1426300 (401920 dedup)");
		CHECK(has(dlc, 401920) && has(dlc, 570660) && has(dlc, 1426300),
		      "merge: union correct");
	}

	// 4) never returns the base appid even if it leaks into a field.
	{
		const std::string wire =
			"\"appinfo\"\n{\n"
			"\t\"extended\"\n\t{\n"
			"\t\t\"listofdlc\"\t\t\"250900,401920\"\n"
			"\t}\n"
			"}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 250900);
		CHECK(!has(dlc, 250900), "excludes the base appid");
		CHECK(has(dlc, 401920) && dlc.size() == 1, "keeps the real dlc only");
	}

	// 5) no DLC info -> empty, no crash.
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"appid\"\t\t\"285900\"\n\t\"depots\"\n\t{\n"
			"\t\t\"285903\"\n\t\t{\n\t\t\t\"config\"\n\t\t\t{\n"
			"\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n\t\t}\n\t}\n}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 285900);
		CHECK(dlc.empty(), "no listofdlc / dlcappid -> empty");
	}

	// 6) empty / garbage input -> empty, no crash.
	{
		CHECK(AppInfoProvision::extractDlcAppIds("", 1).empty(),
		      "empty input -> empty");
		CHECK(AppInfoProvision::extractDlcAppIds("listofdlc no quotes here", 1).empty(),
		      "garbage without quoted value -> empty");
	}

	// 7) ignores a zero id in listofdlc (trailing comma / blank field).
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"extended\"\n\t{\n"
			"\t\t\"listofdlc\"\t\t\"401920,,0,570660\"\n\t}\n}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 250900);
		CHECK(dlc.size() == 2 && has(dlc, 401920) && has(dlc, 570660),
		      "skips empty/zero fields in listofdlc");
	}

	// 8) A content DLC whose depot was rejected (for example because the
	// Lua did not provide its decryption key) must also be removed from
	// extended.listofdlc.  Otherwise PackagePatch advertises ownership and
	// Steam independently plans the rejected depot, ending in
	// "Content still encrypted".
	{
		const std::unordered_set<uint32_t> unsupported = {4229450};
		std::size_t removed = 0;
		const std::string filtered =
			AppInfoProvision::filterUnsupportedDlcAppIds(
				"4173830,4229450,4556380", unsupported, &removed);

		CHECK(filtered == "4173830,4556380",
		      "filter: removes unsupported content DLC only");
		CHECK(removed == 1, "filter: reports one removed DLC");

		// 4556380 represents a virtual DLC entry (no manifests), which does
		// not need a depot key and must remain advertised.
		const std::string wire =
			"\"appinfo\"\n{\n"
			"\t\"extended\"\n\t{\n"
			"\t\t\"listofdlc\"\t\t\"" + filtered + "\"\n"
			"\t}\n"
			"\t\"depots\"\n\t{\n"
			"\t\t\"4173830\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"4173830\"\n\t\t}\n"
			"\t\t\"4556380\"\n\t\t{\n\t\t\t\"dlcappid\"\t\t\"4556380\"\n\t\t}\n"
			"\t}\n"
			"}\n";
		auto dlc = AppInfoProvision::extractDlcAppIds(wire, 2968420);
		CHECK(!has(dlc, 4229450),
		      "filter: rejected content DLC is no longer injected as owned");
		CHECK(has(dlc, 4173830) && has(dlc, 4556380),
		      "filter: keyed and virtual DLCs remain injected");
	}

	// 9) If every advertised DLC is unsupported, produce an empty list
	// rather than leaving separators or a stale appid behind.
	{
		const std::unordered_set<uint32_t> unsupported = {4229450};
		std::size_t removed = 0;
		const std::string filtered =
			AppInfoProvision::filterUnsupportedDlcAppIds(
				"4229450", unsupported, &removed);
		CHECK(filtered.empty(), "filter: all unsupported -> empty list");
		CHECK(removed == 1, "filter: all unsupported count is correct");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
