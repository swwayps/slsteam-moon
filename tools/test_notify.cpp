// Standalone test for the desktop-notification command builder: the
// notify-send command, its shell escaping, the per-category throttle and
// the gamepad queue.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_notify.cpp -o /tmp/test_notify && /tmp/test_notify

#include "../src/notify.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool contains(const std::string& hay, const std::string& needle)
{
	return hay.find(needle) != std::string::npos;
}

int main()
{
	// A NUL in a body is dropped: system() would stop at it and lose the
	// closing quote.
	{
		std::string withNull = "Loaded successfully";
		withNull.push_back('\0');
		const std::string cmd =
			Notify::buildCommandRaw("SLSsteam-moon", withNull, 10000, "normal");
		const std::string asExecuted(cmd.c_str());
		CHECK(asExecuted == cmd,
		      "command carries no embedded null (would truncate in system())");
		CHECK(!asExecuted.empty() && asExecuted.back() == '"',
		      "command ends with the closing quote (not truncated mid-word)");
	}

	// 9) buildCommandRaw: the user-facing popup path takes an explicit title,
	//    body, timeout and urgency (the catalog layer decides those).
	{
		const std::string cmd =
			Notify::buildCommandRaw("SLSsteam-moon", "servers busy", 30000, "normal");
		CHECK(contains(cmd, "notify-send"),  "raw cmd invokes notify-send");
		CHECK(contains(cmd, "-t 30000"),     "raw cmd carries the given timeout");
		CHECK(contains(cmd, "-u \"normal\""),"raw cmd carries the given urgency");
		CHECK(contains(cmd, "SLSsteam-moon"),"raw cmd carries the title");
		CHECK(contains(cmd, "servers busy"), "raw cmd carries the body");

		const std::string inj =
			Notify::buildCommandRaw("t", "x`id`$(id)\"q", 1000, "normal");
		CHECK(contains(inj, "\\`id\\`") && contains(inj, "\\$(id)") &&
		      contains(inj, "\\\"q"),
		      "raw cmd escapes shell metacharacters in the body");
	}

	// 10) NotifyThrottle: anti-spam. A multi-DLC title can fire the SAME
	//     failure (e.g. "content servers unavailable") for a dozen depots
	//     within seconds. The throttle collapses a burst per category into
	//     ONE popup, counting the suppressed ones so the next allowed popup
	//     can mention them. Distinct categories are independent.
	{
		Notify::NotifyThrottle thr;
		thr.cooldownMs = 60000;
		int suppressed = -1;

		// First occurrence of category 1 emits immediately, nothing suppressed.
		CHECK(thr.allow(1, /*nowMs=*/0, suppressed) && suppressed == 0,
		      "first occurrence emits (suppressed=0)");

		// A burst within the cooldown window is suppressed.
		CHECK(!thr.allow(1, 1000, suppressed),  "burst #1 suppressed");
		CHECK(!thr.allow(1, 2000, suppressed),  "burst #2 suppressed");
		CHECK(!thr.allow(1, 3000, suppressed),  "burst #3 suppressed");

		// A different category is not affected by category 1's cooldown.
		CHECK(thr.allow(2, 3000, suppressed) && suppressed == 0,
		      "distinct category emits independently");

		// After the cooldown elapses, category 1 emits again and reports the
		// three that were suppressed in between.
		CHECK(thr.allow(1, 61000, suppressed) && suppressed == 3,
		      "post-cooldown emit reports the 3 suppressed occurrences");

		// The suppressed counter resets after an allowed emit.
		CHECK(!thr.allow(1, 61500, suppressed), "next burst suppressed again");
		CHECK(thr.allow(1, 122000, suppressed) && suppressed == 1,
		      "counter reset: only 1 suppressed since last emit");
	}

	// 11) Gamepad UI transport: notifications are handed to the Lumen sidecar
	//     through an atomic JSON event. This is intentionally independent of a
	//     desktop notification daemon, so SteamOS/Bazzite-style sessions can
	//     render the message inside Steam itself.
	{
		const std::string payload = Notify::buildGamepadPayload(
			"SLSsteam-moon", "quoted \"body\"\nsecond line", 30000, 123456789);
		CHECK(contains(payload, "\"version\":1"),
		      "gamepad payload carries its schema version");
		CHECK(contains(payload, "\"created_ms\":123456789"),
		      "gamepad payload carries its creation time");
		CHECK(contains(payload, "quoted \\\"body\\\"\\nsecond line"),
		      "gamepad payload JSON-escapes quotes and newlines");
		CHECK(contains(payload, "\"timeout_ms\":30000"),
		      "gamepad payload carries the requested timeout");

		const auto base = std::filesystem::temp_directory_path() /
			("sls-notify-test-" + std::to_string(Notify::processId()));
		std::error_code ec;
		std::filesystem::remove_all(base, ec);
		CHECK(Notify::enqueueGamepadEvent(base.string(), "SLSsteam-moon",
		      "ready", 10000, 123456790),
		      "gamepad event is queued atomically");

		int jsonFiles = 0;
		int temporaryFiles = 0;
		std::string queued;
		for (const auto& entry : std::filesystem::directory_iterator(base))
		{
			if (entry.path().extension() == ".json")
			{
				++jsonFiles;
				std::ifstream in(entry.path());
				queued.assign(std::istreambuf_iterator<char>(in), {});
			}
			if (entry.path().extension() == ".tmp") ++temporaryFiles;
		}
		CHECK(jsonFiles == 1, "queue publishes exactly one JSON event");
		CHECK(temporaryFiles == 0, "queue leaves no partially-written event");
		CHECK(contains(queued, "\"body\":\"ready\""),
		      "published event preserves its body");
		std::filesystem::remove_all(base, ec);
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
