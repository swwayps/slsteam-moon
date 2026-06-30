// Standalone test for the synthetic-app marker + PICS strip logic
// (src/feats/synthmark.hpp).
//
// Why this exists
// ---------------
// Token-locked titles (e.g. Risk of Rain 2) have their PICS product-info
// access token DENIED, so Steam's runtime RequestAppInfoUpdate comes back
// with an EMPTY buffer.  When that empty refresh lands, Steam overwrites
// the depots + installdir we synthesized into appinfo at startup -> the
// install dialog drops to 0 B and fails with "Invalid install path".
//
// The fix keeps Steam from ever re-fetching those apps: provisioning marks
// an app "synthetic" when it had to rebuild the depots from local
// manifests, and the outgoing-PICS hook strips marked apps from Steam's
// product-info request so the startup splice is never clobbered.  The mark
// is PERSISTED (a marker file) because Steam re-execs setup() several times
// per boot and the surviving process may hit the provisioning cache and
// skip synthesis — an in-memory set would be empty there.
//
// This pins down the marker round-trip (persisted, survives a fresh load)
// and the pure strip-index logic.  The protobuf request edit lives in
// apps.cpp::sendPICSInfoRequest.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_synthmark.cpp -o /tmp/test_synthmark && /tmp/test_synthmark

#include "../src/feats/synthmark.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	namespace fs = std::filesystem;
	const std::string dir = (fs::temp_directory_path() /
	                         ("slsteam_synthmark_" + std::to_string(::getpid()))).string();
	fs::create_directories(dir);

	// --- marker round-trip -------------------------------------------------

	CHECK(!SynthMark::isMarked(dir, 632360), "unmarked app reads as not synthetic");

	CHECK(SynthMark::mark(dir, 632360), "mark succeeds");
	CHECK(SynthMark::isMarked(dir, 632360), "marked app reads as synthetic");
	CHECK(!SynthMark::isMarked(dir, 111), "a different app is still not synthetic");

	// Persisted: a fresh load (new process would do this) sees the mark.
	{
		auto all = SynthMark::loadAll(dir);
		CHECK(all.count(632360) == 1, "loadAll picks up the persisted mark");
		CHECK(all.count(111) == 0, "loadAll excludes unmarked apps");
	}

	// Marking is idempotent.
	CHECK(SynthMark::mark(dir, 632360), "re-mark is a no-op success");
	CHECK(SynthMark::loadAll(dir).size() == 1, "no duplicate markers");

	// A second app.
	SynthMark::mark(dir, 250900);
	{
		auto all = SynthMark::loadAll(dir);
		CHECK(all.size() == 2 && all.count(250900), "second mark recorded");
	}

	// Unmark (remove-game cleanup).
	CHECK(SynthMark::unmark(dir, 632360), "unmark succeeds");
	CHECK(!SynthMark::isMarked(dir, 632360), "unmarked app no longer synthetic");
	CHECK(SynthMark::isMarked(dir, 250900), "other app's mark untouched");

	// Missing dir must not throw / must read as empty.
	CHECK(!SynthMark::isMarked(dir + "_nope", 1), "isMarked on missing dir -> false");
	CHECK(SynthMark::loadAll(dir + "_nope").empty(), "loadAll on missing dir -> empty");

	// --- stripIndices (pure) ----------------------------------------------

	auto synthetic = [](uint32_t a) { return a == 632360 || a == 632361; };

	// Returned indices are DESCENDING so the caller can delete in place.
	{
		std::vector<uint32_t> req = {10, 632360, 20, 632361};
		auto idx = SynthMark::stripIndices(req, synthetic);
		CHECK((idx == std::vector<int>{3, 1}), "synthetic indices returned descending");
	}

	// Nothing synthetic -> nothing to strip.
	{
		std::vector<uint32_t> req = {10, 20, 30};
		CHECK(SynthMark::stripIndices(req, synthetic).empty(),
		      "no synthetic apps -> empty strip list");
	}

	// All synthetic -> strip everything (descending).
	{
		std::vector<uint32_t> req = {632360, 632361};
		auto idx = SynthMark::stripIndices(req, synthetic);
		CHECK((idx == std::vector<int>{1, 0}), "all synthetic -> all indices descending");
	}

	std::error_code ec;
	fs::remove_all(dir, ec);

	if (g_failures == 0) std::printf("\nAll synthmark tests passed.\n");
	else                 std::printf("\n%d synthmark test(s) FAILED.\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
