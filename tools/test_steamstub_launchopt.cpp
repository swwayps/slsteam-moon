// Standalone test for the pure `--steamless` launch-option helpers
// (src/feats/steamstub_launchopt.hpp).
//
// Why this exists
// ---------------
// SteamStub stripping (feats/steamstub.cpp) is opt-in: a game is only handed
// to Steamless when its Steam launch options carry a `-steamless` /
// `--steamless` token. The decision reads the app's LaunchOptions out of a
// localconfig.vdf blob. The parse has two sharp edges this test pins down:
//   * it MUST be scoped to the "apps" section — an appid that also appears
//     elsewhere in the file (e.g. a binary rich-presence blob keyed by appid)
//     must not be mistaken for the app block;
//   * the token match MUST respect word boundaries so a path like
//     "/opt/no-steamlessness" never flips the opt-in on.
// The IO (finding/reading localconfig.vdf) lives in steamstub.cpp and is
// exercised on the VM; the pure logic is what this test locks down.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_steamstub_launchopt.cpp -o /tmp/test_steamstub_launchopt && /tmp/test_steamstub_launchopt

#include "../src/feats/steamstub_launchopt.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using SteamlessLaunchOpt::parseAppLaunchOptions;
using SteamlessLaunchOpt::requestsSteamless;

// A realistic localconfig.vdf shape: nested UserLocalConfigStore -> ... ->
// apps, two app blocks, plus a same-numbered appid living OUTSIDE apps as a
// binary blob (the shape seen in the wild that must NOT be matched).
static std::string localConfig()
{
	return
		"\"UserLocalConfigStore\"\n{\n"
		"\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n"
		"\t\t\t\t\"apps\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"250900\"\n\t\t\t\t\t{\n"
		"\t\t\t\t\t\t\"LastPlayed\"\t\t\"1700000000\"\n"
		"\t\t\t\t\t\t\"LaunchOptions\"\t\t\"game-performance mangohud %command%\"\n"
		"\t\t\t\t\t}\n"
		"\t\t\t\t\t\"1868140\"\n\t\t\t\t\t{\n"
		"\t\t\t\t\t\t\"LaunchOptions\"\t\t\"--steamless %command%\"\n"
		"\t\t\t\t\t}\n"
		"\t\t\t\t\t\"620\"\n\t\t\t\t\t{\n"
		"\t\t\t\t\t\t\"LaunchOptions\"\t\t\"\"\n"
		"\t\t\t\t\t}\n"
		"\t\t\t\t}\n"
		"\t\t\t}\n\t\t}\n\t}\n"
		// A binary blob keyed by the SAME appid, OUTSIDE apps — must be ignored.
		"\t\"friends\"\n\t{\n"
		"\t\t\"250900\"\t\t\"00696e7465726e616c00 --steamless\"\n"
		"\t}\n"
		"}\n";
}

int main()
{
	const std::string cfg = localConfig();

	// 1) Scoped extraction: the apps-block value wins for each appid.
	CHECK(parseAppLaunchOptions(cfg, 250900) == "game-performance mangohud %command%",
	      "parse: apps-scoped LaunchOptions for 250900");
	CHECK(parseAppLaunchOptions(cfg, 1868140) == "--steamless %command%",
	      "parse: apps-scoped LaunchOptions for 1868140");

	// 2) Empty value block is returned as empty string.
	CHECK(parseAppLaunchOptions(cfg, 620).empty(),
	      "parse: empty LaunchOptions -> empty");

	// 3) Scoping guard: 250900 also appears OUTSIDE apps (with --steamless in
	//    a blob value); the parser must return the apps value, not the blob.
	CHECK(parseAppLaunchOptions(cfg, 250900).find("--steamless") == std::string::npos,
	      "parse: does not leak the out-of-apps blob value");

	// 4) Absent appid -> empty (even if the number appears elsewhere).
	CHECK(parseAppLaunchOptions(cfg, 999999).empty(),
	      "parse: absent appid -> empty");

	// 5) Superstring appid must not match (2509000 != 250900).
	CHECK(parseAppLaunchOptions(cfg, 2509000).empty(),
	      "parse: superstring appid (2509000) not matched");

	// 6) No apps section at all -> empty, and empty/garbage input is safe.
	CHECK(parseAppLaunchOptions("\"x\"\n{\n}\n", 250900).empty(),
	      "parse: no apps section -> empty");
	CHECK(parseAppLaunchOptions("", 250900).empty(),
	      "parse: empty input -> empty");

	// 7) Backslash-escaped quotes in the value are unescaped.
	{
		const std::string cfg2 =
			"\"apps\"\n{\n\t\"10\"\n\t{\n"
			"\t\t\"LaunchOptions\"\t\t\"WINEDLLOVERRIDES=\\\"steam_api64=n,b\\\" %command% -steamless\"\n"
			"\t}\n}\n";
		const std::string v = parseAppLaunchOptions(cfg2, 10);
		CHECK(v == "WINEDLLOVERRIDES=\"steam_api64=n,b\" %command% -steamless",
		      "parse: unescapes \\\" in the value");
		CHECK(requestsSteamless(v), "parse+token: -steamless after escaped value is detected");
	}

	// 8) requestsSteamless: both spellings, boundaries, and rejections.
	CHECK(requestsSteamless("--steamless"),           "token: bare --steamless");
	CHECK(requestsSteamless("-steamless"),            "token: bare -steamless");
	CHECK(requestsSteamless("%command% --steamless"), "token: --steamless at end");
	CHECK(requestsSteamless("--steamless %command%"), "token: --steamless at start-ish");
	CHECK(requestsSteamless("mangohud\t--steamless"), "token: tab-separated");
	CHECK(!requestsSteamless(""),                     "token: empty -> false");
	CHECK(!requestsSteamless("game-performance %command%"), "token: unrelated opts -> false");
	CHECK(!requestsSteamless("/opt/no-steamlessness"), "token: substring not matched");
	CHECK(!requestsSteamless("--steamlessX"),         "token: trailing junk not matched");
	CHECK(!requestsSteamless("x--steamlessy"),        "token: embedded not matched");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
