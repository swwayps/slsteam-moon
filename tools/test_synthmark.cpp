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
// This pins down the persisted marker round-trip, managed-app filtering,
// pure strip-budget policy, and reversible quarantine of app-scoped cache
// artifacts. The protobuf request edit lives in apps.cpp::sendPICSInfoRequest.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_synthmark.cpp -o /tmp/test_synthmark && /tmp/test_synthmark

#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/manifestid.hpp"
#include "../src/feats/synthmark.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <unistd.h>

static_assert(std::is_same_v<decltype(&AppInfoProvision::forgetApp),
                             bool (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&AppInfoProvision::forgetManagedSourceApp),
                             bool (*)(uint32_t)>);

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

	// Managed-app scope: an orphaned synthetic marker must not be stripped.
	{
		auto syntheticWithOrphan = [](uint32_t a) {
			return a == 632360 || a == 632361 || a == 4496490;
		};
		auto managed = [](uint32_t a) {
			return a == 632360 || a == 632361;
		};
		std::vector<uint32_t> req = {632360, 4496490, 632361, 10};
		auto idx = SynthMark::stripIndices(req, syntheticWithOrphan, managed);
		CHECK((idx == std::vector<int>{2, 0}),
		      "only managed synthetic apps are selected for stripping");
	}

	// Install-state gate: an unresolved app-manager interface preserves the
	// early-boot stripping behavior, while a fully installed app is safe to
	// refresh and must not be stripped.
	CHECK(!SynthMark::installStateAllowsStrip(false, true, false, false),
	      "non-synthetic app is never strip-eligible");
	CHECK(!SynthMark::installStateAllowsStrip(true, false, false, false),
	      "unmanaged synthetic app is never strip-eligible");
	CHECK(SynthMark::installStateAllowsStrip(true, true, false, true),
	      "unresolved app manager keeps stripping enabled");
	CHECK(SynthMark::installStateAllowsStrip(true, true, true, false),
	      "not-fully-installed synthetic app remains strip-eligible");
	CHECK(!SynthMark::installStateAllowsStrip(true, true, true, true),
	      "fully-installed synthetic app is not strip-eligible");

	// Runtime protection must never expire into a destructive empty response.
	// The AppInfoState skip bit prevents the updater retry loop; this outgoing
	// filter remains the final authority boundary if Steam asks anyway.
	std::ifstream appsSource("src/feats/apps.cpp");
	const std::string appsText(
		(std::istreambuf_iterator<char>(appsSource)),
		std::istreambuf_iterator<char>());
	CHECK(appsText.find("g_synthStripBudget") == std::string::npos,
	      "runtime synthetic protection has no expiring strip budget");
	CHECK(appsText.find("strip cap tripped") == std::string::npos,
	      "runtime can never fall through to clobber after a cap event");

	// --- app/depot relation tracking ---------------------------------------

	ManifestId::AppDepotIndex relations;
	relations.add(321, 987654);
	relations.add(321, 876543);
	relations.add(322, 876543);
	relations.add(321, 987654); // idempotent duplicate
	CHECK((relations.depotsForApp(321) ==
	       std::vector<uint32_t>{876543, 987654}),
	      "relation index records and sorts an app's depots");
	const auto released = relations.releaseApp(321);
	CHECK((released == std::vector<uint32_t>{987654}),
	      "relation release returns only depots unique to the removed app");
	CHECK((relations.depotsForApp(322) == std::vector<uint32_t>{876543}),
	      "shared depot relation remains owned by the other app");
	CHECK(relations.releaseApp(321).empty(),
	      "releasing an already forgotten app is a no-op");
	relations.add(321, 987654);
	CHECK((relations.depotsForApp(321) == std::vector<uint32_t>{987654}),
	      "relation index accepts the app again after a hot-add");
	const auto editedReleased = relations.replaceApp(
		321, std::vector<uint32_t>{876543});
	CHECK((editedReleased == std::vector<uint32_t>{987654}),
	      "relation edit reports depots no longer owned");
	CHECK((relations.depotsForApp(321) == std::vector<uint32_t>{876543}),
	      "relation index reconciles depots removed by a script edit");

	std::ifstream manifestSource("src/feats/manifestid.cpp");
	const std::string manifestText(
		(std::istreambuf_iterator<char>(manifestSource)),
		std::istreambuf_iterator<char>());
	CHECK(manifestText.find("if (g_importDone) return") == std::string::npos,
	      "manifest importer can rescan after a hot-add");
	CHECK(manifestText.find("retireDepots") != std::string::npos,
	      "manifest importer retires pins removed by a script edit");

	// --- reversible orphan quarantine --------------------------------------

	const fs::path quarantineDir = fs::path(dir + "_quarantine");
	fs::create_directories(quarantineDir);
	auto touch = [](const fs::path& path) {
		std::ofstream file(path);
		file << "cache";
	};
	const std::vector<std::string> managedFiles = {
		"picsbuffer_100.bin", "picsbuffer_100.yaml", "synthetic_100",
		"terminal_100.state",
	};
	for (const auto& name : managedFiles) touch(quarantineDir / name);

	const std::vector<std::string> orphanFiles = {
		"picsbuffer_200.bin", "picsbuffer_200.yaml", "synthetic_200",
		"terminal_200.state",
	};
	for (const auto& name : orphanFiles) touch(quarantineDir / name);
	// These caches are not app-scoped: manifestid_<n> uses a depot id,
	// while ticket_<n> needs its own ownership relation.  The generic sweep
	// must leave both for the future app-aware cleanup API.
	touch(quarantineDir / "manifestid_200.yaml");
	touch(quarantineDir / "ticket_200.yaml");
	touch(quarantineDir / "not-a-per-app-cache");

	const std::unordered_set<uint32_t> managedIds = {100};
	auto records = SynthMark::quarantineOrphans(
		quarantineDir.string(), managedIds, ".orphan-test");
	CHECK(records.size() == orphanFiles.size(),
	      "all known orphan cache artifacts are quarantined");
	bool moved = true;
	for (const auto& record : records)
	{
		moved = moved && !fs::exists(record.original) && fs::exists(record.quarantined);
	}
	CHECK(moved, "quarantine renames artifacts without deleting them");
	CHECK(fs::exists(quarantineDir / "picsbuffer_100.bin") &&
	      fs::exists(quarantineDir / "synthetic_100"),
	      "managed cache artifacts remain in place");
	CHECK(fs::exists(quarantineDir / "not-a-per-app-cache"),
	      "unrecognized files remain untouched");
	CHECK(fs::exists(quarantineDir / "manifestid_200.yaml") &&
	      fs::exists(quarantineDir / "ticket_200.yaml"),
	      "depot-scoped and relation-scoped caches remain untouched");

	bool restored = true;
	for (const auto& record : records)
		restored = SynthMark::restoreQuarantined(record) && restored;
	CHECK(restored, "quarantined artifacts can be restored");
	bool roundTrip = true;
	for (const auto& name : orphanFiles)
		roundTrip = roundTrip && fs::exists(quarantineDir / name);
	CHECK(roundTrip, "restored orphan artifacts return to their original paths");

	// --- explicit per-app cleanup quarantine ------------------------------

	const uint32_t forgottenApp = 321;
	const std::vector<std::string> forgottenFiles = {
		"picsbuffer_321.bin", "picsbuffer_321.yaml", "synthetic_321",
		"terminal_321.state",
		"ticket_321.yaml", "encryptedTicket_321.yaml",
		"manifestid_987654.yaml",
	};
	for (const auto& name : forgottenFiles) touch(quarantineDir / name);
	// The app id and a shared depot id are not safe manifest catalog names for
	// this app. Only the relation index's unique depot is eligible here.
	touch(quarantineDir / "manifestid_321.yaml");
	touch(quarantineDir / "manifestid_876543.yaml");
	touch(quarantineDir / "picsbuffer_322.bin");
	// A stale destination from an earlier same-second cleanup must not cause
	// the live artifact to be skipped or overwrite the old quarantine.
	touch(quarantineDir / "picsbuffer_321.bin.forgotten-test");
	{
		std::ofstream sentinel(quarantineDir /
		                       "picsbuffer_321.bin.forgotten-test",
		                       std::ios::trunc);
		sentinel << "sentinel";
	}

	auto forgotten = SynthMark::quarantineAppArtifacts(
		quarantineDir.string(), forgottenApp, std::vector<uint32_t>{987654},
		".forgotten-test");
	CHECK(forgotten.size() == forgottenFiles.size(),
	      "explicit cleanup finds app and relation-scoped artifacts");
	bool forgottenMoved = true;
	for (const auto& record : forgotten)
	{
		forgottenMoved = forgottenMoved && !fs::exists(record.original) &&
		                 fs::exists(record.quarantined);
	}
	CHECK(forgottenMoved, "explicit cleanup quarantines instead of deleting");
	std::ifstream sentinel(quarantineDir /
	                       "picsbuffer_321.bin.forgotten-test");
	std::string sentinelContents;
	sentinel >> sentinelContents;
	CHECK(sentinelContents == "sentinel",
	      "collision preserves the existing quarantine contents");
	CHECK(fs::exists(quarantineDir / "picsbuffer_322.bin"),
	      "explicit cleanup leaves another app untouched");
	CHECK(fs::exists(quarantineDir / "manifestid_321.yaml") &&
	      fs::exists(quarantineDir / "manifestid_876543.yaml"),
	      "app-id and shared depot manifest pins remain untouched");
	bool forgottenRestored = true;
	for (const auto& record : forgotten)
		forgottenRestored = SynthMark::restoreQuarantined(record) && forgottenRestored;
	CHECK(forgottenRestored, "explicitly quarantined artifacts can be restored");

	// Managed-source removal invalidates appinfo artifacts but retains ticket
	// state and synthetic-PICS protection for a compatibility-active app so a
	// later process can reload it without clobbering the live appinfo entry.
	auto managedOnly = SynthMark::quarantineAppArtifacts(
		quarantineDir.string(), forgottenApp, std::vector<uint32_t>{987654},
		".managed-source", /*includeTicketArtifacts=*/false,
		/*includeSyntheticMarker=*/false);
	CHECK(managedOnly.size() == forgottenFiles.size() - 3,
	      "managed-only cleanup excludes ownership tickets and synthetic marker");
	CHECK(fs::exists(quarantineDir / "synthetic_321"),
	      "managed-only cleanup retains synthetic-PICS protection marker");
	CHECK(fs::exists(quarantineDir / "ticket_321.yaml") &&
	      fs::exists(quarantineDir / "encryptedTicket_321.yaml"),
	      "managed-only cleanup retains ticket artifacts in place");
	bool managedOnlyRestored = true;
	for (const auto& record : managedOnly)
		managedOnlyRestored = SynthMark::restoreQuarantined(record) && managedOnlyRestored;
	CHECK(managedOnlyRestored,
	      "managed-only quarantined artifacts can be restored");

	// Exhaust every no-clobber retry destination for one artifact. A cleanup
	// caller must be able to distinguish this partial move from success.
	const uint32_t blockedApp = 326;
	touch(quarantineDir / "picsbuffer_326.bin");
	for (unsigned int attempt = 0; attempt < 1024; ++attempt)
	{
		const auto suffix = attempt == 0
			? std::string{".blocked"}
			: ".blocked." + std::to_string(attempt);
		touch(quarantineDir / ("picsbuffer_326.bin" + suffix));
	}
	auto blocked = SynthMark::quarantineAppArtifacts(
		quarantineDir.string(), blockedApp, std::vector<uint32_t>{}, ".blocked");
	CHECK(blocked.empty(), "exhausted quarantine destinations report no move");
	CHECK(SynthMark::hasAppArtifacts(
		      quarantineDir.string(), blockedApp, std::vector<uint32_t>{}),
	      "remaining app artifact is observable after partial quarantine");

	const uint32_t blockedTerminalApp = 327;
	touch(quarantineDir / "terminal_327.state");
	for (unsigned int attempt = 0; attempt < 1024; ++attempt)
	{
		const auto suffix = attempt == 0
			? std::string{".blocked-terminal"}
			: ".blocked-terminal." + std::to_string(attempt);
		touch(quarantineDir / ("terminal_327.state" + suffix));
	}
	auto blockedTerminal = SynthMark::quarantineAppArtifacts(
		quarantineDir.string(), blockedTerminalApp, std::vector<uint32_t>{},
		".blocked-terminal");
	CHECK(blockedTerminal.empty(),
	      "blocked terminal sidecar reports no successful move");
	CHECK(SynthMark::hasAppArtifacts(
		      quarantineDir.string(), blockedTerminalApp, std::vector<uint32_t>{}),
	      "remaining terminal sidecar makes cleanup incomplete");

	std::error_code ec;
	fs::remove_all(quarantineDir, ec);
	fs::remove_all(dir, ec);

	if (g_failures == 0) std::printf("\nAll synthmark tests passed.\n");
	else                 std::printf("\n%d synthmark test(s) FAILED.\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
