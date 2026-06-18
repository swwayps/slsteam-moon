// Standalone test for the pure achievement-spoof helpers.
//
// The native achievement flow (ported from the LumaCore/OpenSteamTool
// packet model, adapted to slsteam-moon's live-protobuf hook model):
//
//   - On an outgoing stats request (CMsgClientGetUserStats / eMsg 818)
//     for an AdditionalApp, we rewrite steam_id_for_user to a real owner
//     of the game so Steam's servers return a populated achievement
//     schema instead of eresult=Fail, and we record the appid as
//     "just spoofed".
//   - eMsg 819 (CMsgClientGetUserStatsResponse) carries no jobid to
//     correlate with the request, so the response handler keys off that
//     per-appid record: a spoofed request -> strip the dummy-account
//     stats and force eresult=OK; a pass-through request (Steam already
//     had a cached schema) -> leave the response alone so Steam keeps
//     its own cache instead of being told the user has 0 unlocks.
//
// SpoofTracker is the pure gate behind that decision; resolveOwnerSteamId
// is the pure owner-selection rule. Both are dependency-free so they can
// be unit-tested with a stock g++, same pattern as feats/dlcids.hpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_achievements.cpp -o /tmp/test_achievements && /tmp/test_achievements

#include "../src/feats/achievements.hpp"

#include <cstdio>
#include <unordered_map>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using Achievements::SpoofTracker;
	using Achievements::resolveOwnerSteamId;

	// 1) a marked appid is consumed exactly once.
	{
		SpoofTracker t(30);
		t.mark(730, 100);
		CHECK(t.consume(730, 101), "marked appid consumes true within ttl");
		CHECK(!t.consume(730, 102), "second consume of same appid is false");
	}

	// 2) an unmarked appid never consumes.
	{
		SpoofTracker t(30);
		CHECK(!t.consume(440, 5), "unmarked appid consumes false");
	}

	// 3) marks are per-appid and independent.
	{
		SpoofTracker t(30);
		t.mark(10, 0);
		t.mark(20, 0);
		CHECK(t.consume(20, 1), "consume appid 20");
		CHECK(t.consume(10, 1), "consume appid 10 independently");
		CHECK(!t.consume(20, 1), "appid 20 already consumed");
	}

	// 4) an expired mark does not consume (ttl boundary is exclusive of
	//    entries strictly older than ttl seconds).
	{
		SpoofTracker t(30);
		t.mark(99, 100);
		CHECK(!t.consume(99, 131), "mark older than ttl is not consumable");
	}

	// 5) a mark exactly at the ttl edge is still live.
	{
		SpoofTracker t(30);
		t.mark(99, 100);
		CHECK(t.consume(99, 130), "mark at exactly ttl seconds is still live");
	}

	// 6) re-marking refreshes the timestamp so it survives past the
	//    original expiry.
	{
		SpoofTracker t(30);
		t.mark(7, 100);
		t.mark(7, 120);
		CHECK(t.consume(7, 145), "re-mark refreshes ttl window");
	}

	// 7) marking prunes stale entries so the map stays bounded.
	{
		SpoofTracker t(30);
		t.mark(1, 0);
		t.mark(2, 0);
		t.mark(3, 100); // far in the future -> 1 and 2 are now stale
		CHECK(t.size() == 1, "stale entries pruned on mark");
		CHECK(!t.consume(1, 100), "pruned entry 1 not consumable");
		CHECK(!t.consume(2, 100), "pruned entry 2 not consumable");
		CHECK(t.consume(3, 100), "fresh entry 3 still consumable");
	}

	// 8) owner resolution: per-app override wins over the default.
	{
		std::unordered_map<uint32_t, uint64_t> perApp{
			{ 730, 76561197960287930ULL },
		};
		const uint64_t def = 76561197960265728ULL;
		CHECK(resolveOwnerSteamId(730, perApp, def) == 76561197960287930ULL,
		      "per-app owner override wins");
		CHECK(resolveOwnerSteamId(440, perApp, def) == def,
		      "appid without override falls back to default");
	}

	// 9) owner resolution with empty override map -> always default.
	{
		std::unordered_map<uint32_t, uint64_t> perApp;
		const uint64_t def = 12345ULL;
		CHECK(resolveOwnerSteamId(1, perApp, def) == def,
		      "empty override map -> default");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
