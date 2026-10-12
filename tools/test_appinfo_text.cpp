// Standalone test for the appinfo text depot reader (src/feats/appinfo_text.hpp).
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_appinfo_text.cpp -o /tmp/test_appinfo_text && /tmp/test_appinfo_text

#include "../src/feats/appinfo_text.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool hasDepot(const std::vector<AppInfoText::DepotGid>& v,
                     uint32_t depotId, uint64_t gid)
{
	return std::any_of(v.begin(), v.end(),
	    [&](const AppInfoText::DepotGid& d) {
	        return d.first == depotId && d.second == gid;
	    });
}

// Windows and Linux base depots plus Linux, Windows and macOS DLC depots.
static std::string mixedWire()
{
	return
		"\"appinfo\"\n{\n"
		"\t\"appid\"\t\t\"250900\"\n"
		"\t\"depots\"\n\t{\n"
		"\t\t\"250902\"\n\t\t{\n"
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"windows\"\n\t\t\t}\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1002\"\n\t\t\t\t\t\"size\"\t\t\"999\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250903\"\n\t\t{\n"
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1003\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250904\"\n\t\t{\n"
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"macos\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"401920\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1004\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250906\"\n\t\t{\n"
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"401920\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1006\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250911\"\n\t\t{\n"
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"windows\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"1426300\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1011\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t}\n}\n";
}

static std::string oneDepot(const std::string& depotId, const std::string& gid)
{
	return "\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
	       "\t\t\"" + depotId + "\"\n\t\t{\n\t\t\t\"manifests\"\n\t\t\t{\n"
	       "\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"" + gid +
	       "\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
}

int main()
{
	// Every depot's public gid, whatever its OS.
	{
		const auto d = AppInfoText::extractDepotsAndGids(mixedWire());
		CHECK(d.size() == 5, "extract: all 5 depots' public gids");
		CHECK(hasDepot(d, 250902, 1002), "extract: windows base depot");
		CHECK(hasDepot(d, 250903, 1003), "extract: linux base depot");
		CHECK(hasDepot(d, 250904, 1004), "extract: macos depot");
		CHECK(hasDepot(d, 250906, 1006), "extract: linux DLC depot");
		CHECK(hasDepot(d, 250911, 1011), "extract: windows-only DLC depot");
		CHECK(d.front().first == 250902 && d.back().first == 250911,
		      "extract: buffer order is kept");
	}

	// Only the `public` branch gid is taken.
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"300\"\n\t\t{\n\t\t\t\"manifests\"\n\t\t\t{\n"
			"\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"7\"\n\t\t\t\t}\n"
			"\t\t\t\t\"beta\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"8\"\n\t\t\t\t}\n"
			"\t\t\t}\n\t\t}\n\t}\n}\n";
		const auto d = AppInfoText::extractDepotsAndGids(wire);
		CHECK(d.size() == 1 && hasDepot(d, 300, 7),
		      "extract: takes public gid, ignores beta branch");
	}

	// A depot without manifests contributes nothing; empty or garbage input is safe.
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"400\"\n\t\t{\n\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n\t\t}\n"
			"\t}\n}\n";
		CHECK(AppInfoText::extractDepotsAndGids(wire).empty(),
		      "extract: depot without manifests -> nothing");
		CHECK(AppInfoText::extractDepotsAndGids("").empty(),
		      "extract: empty input -> empty");
		CHECK(AppInfoText::extractDepotsAndGids("no depots here").empty(),
		      "extract: no depots block -> empty");
	}

	// Numbers that do not fit are ignored instead of wrapping.
	{
		CHECK(AppInfoText::extractDepotsAndGids(oneDepot("4294967296", "5")).empty(),
		      "extract: a depot id beyond 32 bits is ignored");
		CHECK(AppInfoText::extractDepotsAndGids(oneDepot("500", "18446744073709551616")).empty(),
		      "extract: a gid beyond 64 bits is ignored");
		const auto d = AppInfoText::extractDepotsAndGids(oneDepot("4294967295", "18446744073709551615"));
		CHECK(d.size() == 1 && hasDepot(d, 4294967295u, 18446744073709551615ull),
		      "extract: the largest depot id and gid are kept");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
