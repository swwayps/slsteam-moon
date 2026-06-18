// Standalone test for the pure Player.GetUserStats wire helpers.
//
// Modern Steam clients fetch a game's achievement schema for the library
// page via the unified service method Player.GetUserStats#1 (eMsg 151),
// NOT the legacy ClientGetUserStats (eMsg 818). For an AdditionalApp the
// account doesn't own server-side, that request comes back empty, so the
// library Achievements tab never appears.
//
// The fix mirrors OpenSteamTool/LumaCore: rewrite the outgoing request's
// steamid to a real owner so the server returns a populated schema, and
// strip the owner's per-stat unlock data from the response so the tab
// shows the achievement list without the owner's progress.
//
// CPlayer_GetUserStats_Request  { uint64 steamid=1; uint32 appid=2;
//                                 bytes sha_schema=3; uint32 crc_stats=4; }
// CPlayer_GetUserStats_Response { bytes sha_schema=1; uint32 crc_stats=2;
//                                 bytes schema=3; repeated Stats stats=4; }
//
// These helpers do raw protobuf wire editing (no generated classes, since
// the player proto isn't compiled into the fork), so they're pure and
// unit-testable with a stock g++, same pattern as feats/dlcids.hpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_playerstats.cpp -o /tmp/test_playerstats && /tmp/test_playerstats

#include "../src/feats/playerstats.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using namespace PlayerStats;

// Helper: append a protobuf varint (for building test inputs).
static void putVarint(std::vector<uint8_t>& out, uint64_t v)
{
	while (v >= 0x80) { out.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
	out.push_back(static_cast<uint8_t>(v));
}

int main()
{
	// 1) read appid (field 2) from a request body, fields in order.
	{
		std::vector<uint8_t> body;
		body.push_back(0x08); putVarint(body, 76561197960265728ULL); // steamid
		body.push_back(0x10); putVarint(body, 2506160);             // appid
		auto appid = parseRequestAppId(body.data(), body.size());
		CHECK(appid.has_value() && *appid == 2506160, "parseRequestAppId reads field 2");
	}

	// 2) read appid when it appears before steamid (field order independence).
	{
		std::vector<uint8_t> body;
		body.push_back(0x10); putVarint(body, 480);                 // appid first
		body.push_back(0x08); putVarint(body, 123456789ULL);        // steamid
		auto appid = parseRequestAppId(body.data(), body.size());
		CHECK(appid.has_value() && *appid == 480, "parseRequestAppId order-independent");
	}

	// 3) request without an appid -> nullopt (don't spoof blind).
	{
		std::vector<uint8_t> body;
		body.push_back(0x08); putVarint(body, 999ULL);              // steamid only
		auto appid = parseRequestAppId(body.data(), body.size());
		CHECK(!appid.has_value(), "parseRequestAppId returns nullopt without appid");
	}

	// 4) parse skips a length-delimited field (sha_schema=3) correctly.
	{
		std::vector<uint8_t> body;
		body.push_back(0x08); putVarint(body, 1ULL);                // steamid
		body.push_back(0x1a); putVarint(body, 4); body.insert(body.end(), {0xDE,0xAD,0xBE,0xEF}); // sha_schema bytes
		body.push_back(0x10); putVarint(body, 730);                 // appid after a bytes field
		auto appid = parseRequestAppId(body.data(), body.size());
		CHECK(appid.has_value() && *appid == 730, "parseRequestAppId skips bytes field");
	}

	// 5) build a spoofed request: exactly steamid(1) + appid(2), nothing else.
	{
		const uint64_t owner = 76561198028121353ULL;
		auto out = buildSpoofedRequest(owner, 2506160);
		// Round-trip: appid must read back, and re-parsing the steamid via a
		// generic walk must equal owner.
		auto appid = parseRequestAppId(out.data(), out.size());
		CHECK(appid.has_value() && *appid == 2506160, "buildSpoofedRequest carries appid");
		auto sid = parseRequestSteamId(out.data(), out.size());
		CHECK(sid.has_value() && *sid == owner, "buildSpoofedRequest carries owner steamid");
		// sha_schema(3) and crc_stats(4) must be absent.
		CHECK(!hasField(out.data(), out.size(), 3), "spoofed request drops sha_schema");
		CHECK(!hasField(out.data(), out.size(), 4), "spoofed request drops crc_stats");
	}

	// 6) strip stats (field 4) from a response, keeping schema (field 3).
	{
		std::vector<uint8_t> resp;
		resp.push_back(0x1a); putVarint(resp, 5); resp.insert(resp.end(), {'h','e','l','l','o'}); // schema=3 "hello"
		resp.push_back(0x22); putVarint(resp, 3); resp.insert(resp.end(), {0x08,0x01,0x02});      // stats=4 (a Stats msg)
		resp.push_back(0x22); putVarint(resp, 2); resp.insert(resp.end(), {0x08,0x07});           // stats=4 (another)
		auto stripped = stripResponseStats(resp.data(), resp.size());
		CHECK(hasField(stripped.data(), stripped.size(), 3), "stripResponseStats keeps schema");
		CHECK(!hasField(stripped.data(), stripped.size(), 4), "stripResponseStats drops stats");
		// schema content preserved.
		auto schema = fieldBytes(stripped.data(), stripped.size(), 3);
		CHECK(schema.size() == 5 && schema[0] == 'h', "stripResponseStats preserves schema bytes");
	}

	// 7) strip is a no-op-shaped passthrough when there are no stats.
	{
		std::vector<uint8_t> resp;
		resp.push_back(0x1a); putVarint(resp, 2); resp.insert(resp.end(), {'h','i'}); // schema only
		auto stripped = stripResponseStats(resp.data(), resp.size());
		CHECK(hasField(stripped.data(), stripped.size(), 3), "no-stats response keeps schema");
		CHECK(!hasField(stripped.data(), stripped.size(), 4), "no-stats response still has no stats");
	}

	// 8) robustness: truncated/garbage input never crashes, returns empty/none.
	{
		const uint8_t bad[] = { 0x1a, 0x7f }; // says 127 bytes follow, but none do
		auto appid = parseRequestAppId(bad, sizeof(bad));
		CHECK(!appid.has_value(), "truncated input -> no appid, no crash");
		auto stripped = stripResponseStats(bad, sizeof(bad));
		CHECK(stripped.empty() || !hasField(stripped.data(), stripped.size(), 4),
		      "truncated response strip -> no crash");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
