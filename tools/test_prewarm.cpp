// Standalone test for the pure manifest pre-warm planning logic
// (src/feats/prewarm.hpp).
//
// Why this exists
// ---------------
// The install-time PICS recv handler stages every depot-with-key
// synchronously, so the FIRST install of an AddedApp downloads fine.
// But Steam PURGES depotcache manifests that aren't in the committed
// mount set, and a later planning pass (user forces a Proton compat
// tool, or Steam re-validates) does NOT trigger a fresh PICS product-
// info request — so those purged windows/DLC manifests are gone and the
// pass falls into BYldRequestDepotManifest -> "Access Denied" -> one
// ~30s retry (proven on the Zorin VM 2026-06-05 with BoI 250900).
//
// The pre-warm runner keeps every AddedApp's depot manifests staged on
// disk in the background so any later planning pass finds them already
// there.  The PURE decision — "given these provisioned appinfo buffers
// and the depots we hold keys for, which (depotId, gid) manifests do we
// keep warm?" — is what this test pins down.  The threading / HTTP /
// disk I/O lives in prewarm.cpp and is exercised on the VM.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_prewarm.cpp -o /tmp/test_prewarm && /tmp/test_prewarm

#include "../src/feats/prewarm.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool hasDepot(const std::vector<Prewarm::DepotGid>& v,
                     uint32_t depotId, uint64_t gid)
{
	return std::any_of(v.begin(), v.end(),
	    [&](const Prewarm::DepotGid& d) {
	        return d.first == depotId && d.second == gid;
	    });
}

// A realistic provisioned appinfo wire-text buffer: a native-Linux base
// depot, a windows base depot, a native-Linux DLC depot, a windows-only
// DLC depot AND a macOS-only DLC depot — the BoI 250900 shape that
// motivated this.  On Linux, Steam runs the native build or the windows
// build under Proton, but NEVER the macOS build, so a macOS depot is dead
// weight we must not try to warm (its CDN manifest fetch 401s and, looping
// every pass, spams a critical popup — observed on the VM 2026-06-05).
static std::string boiWire()
{
	return
		"\"appinfo\"\n{\n"
		"\t\"appid\"\t\t\"250900\"\n"
		"\t\"depots\"\n\t{\n"
		"\t\t\"250902\"\n\t\t{\n"                          // windows base
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"windows\"\n\t\t\t}\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1002\"\n\t\t\t\t\t\"size\"\t\t\"999\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250903\"\n\t\t{\n"                          // linux base
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1003\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250904\"\n\t\t{\n"                          // macOS DLC (must be skipped)
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"macos\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"401920\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1004\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250906\"\n\t\t{\n"                          // linux DLC (Afterbirth)
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"401920\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1006\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t\t\"250911\"\n\t\t{\n"                          // windows-only DLC (Repentance)
		"\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"windows\"\n\t\t\t}\n"
		"\t\t\t\"dlcappid\"\t\t\"1426300\"\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1011\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
		"\t}\n}\n";
}

int main()
{
	// 1) extractDepotsAndGids stays OS-agnostic: it pulls the public gid
	//    for EVERY depot it parses (the OS filtering happens later, in
	//    planStageTargets).  5 depots in the fixture.
	{
		auto d = Prewarm::extractDepotsAndGids(boiWire());
		CHECK(d.size() == 5, "extract: all 5 depots' public gids");
		CHECK(hasDepot(d, 250902, 1002), "extract: windows base depot");
		CHECK(hasDepot(d, 250903, 1003), "extract: linux base depot");
		CHECK(hasDepot(d, 250904, 1004), "extract: macos depot (still parsed)");
		CHECK(hasDepot(d, 250906, 1006), "extract: linux DLC depot");
		CHECK(hasDepot(d, 250911, 1011), "extract: windows-only DLC depot");
	}

	// 2) extractDepotsAndGids: only the `public` branch gid is taken,
	//    a `beta`/other branch gid is ignored.
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"300\"\n\t\t{\n\t\t\t\"manifests\"\n\t\t\t{\n"
			"\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"7\"\n\t\t\t\t}\n"
			"\t\t\t\t\"beta\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"8\"\n\t\t\t\t}\n"
			"\t\t\t}\n\t\t}\n\t}\n}\n";
		auto d = Prewarm::extractDepotsAndGids(wire);
		CHECK(d.size() == 1 && hasDepot(d, 300, 7),
		      "extract: takes public gid, ignores beta branch");
	}

	// 3) extractDepotsAndGids: a depot with no manifests block contributes
	//    nothing (e.g. a shared-install / dlconly stub), and empty/garbage
	//    input is safe.
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"400\"\n\t\t{\n\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"linux\"\n\t\t\t}\n\t\t}\n"
			"\t}\n}\n";
		CHECK(Prewarm::extractDepotsAndGids(wire).empty(),
		      "extract: depot without manifests -> nothing");
		CHECK(Prewarm::extractDepotsAndGids("").empty(),
		      "extract: empty input -> empty");
		CHECK(Prewarm::extractDepotsAndGids("no depots here").empty(),
		      "extract: no depots block -> empty");
	}

	// 4) planStageTargets: keeps ONLY depots we hold a key for (mirrors
	//    the runtime DepotKey check) — other-OS depots without a key are
	//    dropped so we don't waste CDN round-trips on undecryptable blobs.
	{
		const std::vector<std::string> wires{ boiWire() };
		// Simulate: we have keys for the linux base + linux DLC only.
		auto hasKey = [](uint32_t depotId) {
			return depotId == 250903 || depotId == 250906;
		};
		auto t = Prewarm::planStageTargets(wires, hasKey);
		CHECK(t.size() == 2, "plan: keeps only keyed depots");
		CHECK(hasDepot(t, 250903, 1003) && hasDepot(t, 250906, 1006),
		      "plan: kept the keyed depots");
		CHECK(!hasDepot(t, 250902, 1002) && !hasDepot(t, 250911, 1011),
		      "plan: dropped keyless depots");
	}

	// 5) planStageTargets: even with keys for ALL depots, the macOS-only
	//    depot (250904) is SKIPPED — Steam on Linux runs the native build
	//    or the windows build under Proton, never macOS, so warming a
	//    macOS manifest only 401s the CDN and (looping) spams a critical
	//    popup (observed on the VM 2026-06-05).  Linux + windows are BOTH
	//    kept (Proton needs the windows depots staged ahead of time).
	{
		const std::vector<std::string> wires{ boiWire() };
		auto allKeys = [](uint32_t) { return true; };
		auto t = Prewarm::planStageTargets(wires, allKeys);
		CHECK(t.size() == 4, "plan: all-keys keeps linux+windows, drops macOS-only");
		CHECK(hasDepot(t, 250902, 1002), "plan: keeps windows base (Proton)");
		CHECK(hasDepot(t, 250903, 1003), "plan: keeps linux base");
		CHECK(hasDepot(t, 250906, 1006), "plan: keeps linux DLC");
		CHECK(hasDepot(t, 250911, 1011), "plan: keeps windows-only DLC (Proton)");
		CHECK(!hasDepot(t, 250904, 1004), "plan: drops macOS-only DLC depot");
	}

	// 6) planStageTargets: dedups the SAME (depotId, gid) seen across
	//    multiple AddedApp buffers (shared depots / repeated entries).
	{
		const std::vector<std::string> wires{ boiWire(), boiWire() };
		auto allKeys = [](uint32_t) { return true; };
		auto t = Prewarm::planStageTargets(wires, allKeys);
		CHECK(t.size() == 4, "plan: dedups identical depots across apps");
	}

	// 7) planStageTargets: a depot with NO oslist is kept (can't tell its
	//    platform — typically a shared/all-OS depot; warming it is safe and
	//    the CDN serves it for the running platform).
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"500\"\n\t\t{\n\t\t\t\"manifests\"\n\t\t\t{\n"
			"\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"55\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
			"\t}\n}\n";
		auto allKeys = [](uint32_t) { return true; };
		auto t = Prewarm::planStageTargets({ wire }, allKeys);
		CHECK(t.size() == 1 && hasDepot(t, 500, 55),
		      "plan: depot with no oslist is kept");
	}

	// 8) planStageTargets: a multi-OS depot listing macos alongside
	//    linux/windows is KEPT (it serves the running platform too).
	{
		const std::string wire =
			"\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			"\t\t\"600\"\n\t\t{\n\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"windows,macos,linux\"\n\t\t\t}\n"
			"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"66\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
			"\t\t\"601\"\n\t\t{\n\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\t\"macos\"\n\t\t\t}\n"
			"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\t\"67\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n"
			"\t}\n}\n";
		auto allKeys = [](uint32_t) { return true; };
		auto t = Prewarm::planStageTargets({ wire }, allKeys);
		CHECK(t.size() == 1 && hasDepot(t, 600, 66) && !hasDepot(t, 601, 67),
		      "plan: keeps multi-OS depot, drops macOS-only sibling");
	}

	// 9) planStageTargets: empty inputs are safe.
	{
		auto allKeys = [](uint32_t) { return true; };
		CHECK(Prewarm::planStageTargets({}, allKeys).empty(),
		      "plan: no buffers -> empty");
		CHECK(Prewarm::planStageTargets({ "" }, allKeys).empty(),
		      "plan: empty buffer -> empty");
	}

	// Partial pins replace only their own public depot; unpinned siblings remain
	// required so readiness cannot hide a missing base/DLC manifest.
	{
		const std::vector<Prewarm::DepotGid> publicTargets{
			{100, 1000}, {101, 1001},
		};
		const std::unordered_map<uint32_t, uint64_t> pins{
			{100, 900}, {102, 902},
		};
		const auto targets = Prewarm::planPinnedStageTargets(publicTargets, pins);
		CHECK(targets.size() == 3, "pins: exact pins plus public siblings are required");
		CHECK(hasDepot(targets, 100, 900) && !hasDepot(targets, 100, 1000),
		      "pins: exact pin replaces that depot's public gid");
		CHECK(hasDepot(targets, 101, 1001) && hasDepot(targets, 102, 902),
		      "pins: unpinned public and pin-only depots are both retained");
	}

	// --- Workshop manifests -------------------------------------------------
	//
	// The workshop depot has depotId == appId (e.g. 250900) and a DYNAMIC
	// per-item manifest gid that is NOT in the appinfo `depots` block — it
	// only appears as the value of `workshopdepot`.  The actual gids live in
	// steamapps/workshop/appworkshop_<appid>.acf, both the currently-
	// installed manifest (WorkshopItemsInstalled) and the latest available
	// (WorkshopItemDetails.latest_manifest).  So neither extractDepotsAndGids
	// nor planStageTargets can see them; we mine the ACF separately and warm
	// (appId, gid) so a workshop update/re-validate finds the manifest on
	// disk and skips BYldRequestDepotManifest (the ~30s first-attempt retry,
	// proven on the Zorin VM 2026-06-05 with BoI 250900).
	{
		// The real BoI 250900 ACF shape (two subscribed items).
		const std::string acf =
			"\"AppWorkshop\"\n{\n"
			"\t\"appid\"\t\t\"250900\"\n"
			"\t\"WorkshopItemsInstalled\"\n\t{\n"
			"\t\t\"3735300204\"\n\t\t{\n"
			"\t\t\t\"size\"\t\t\"8160\"\n"
			"\t\t\t\"manifest\"\t\t\"7988188911654056057\"\n\t\t}\n"
			"\t\t\"3735369753\"\n\t\t{\n"
			"\t\t\t\"size\"\t\t\"208273\"\n"
			"\t\t\t\"manifest\"\t\t\"9070506685956686971\"\n\t\t}\n"
			"\t}\n"
			"\t\"WorkshopItemDetails\"\n\t{\n"
			"\t\t\"3735300204\"\n\t\t{\n"
			"\t\t\t\"manifest\"\t\t\"7988188911654056057\"\n"
			"\t\t\t\"latest_manifest\"\t\t\"7988188911654056057\"\n\t\t}\n"
			"\t\t\"3735369753\"\n\t\t{\n"
			"\t\t\t\"manifest\"\t\t\"9070506685956686971\"\n"
			"\t\t\t\"latest_manifest\"\t\t\"9070506685956686971\"\n\t\t}\n"
			"\t}\n}\n";
		auto w = Prewarm::extractWorkshopManifests(acf, 250900);
		CHECK(w.size() == 2, "workshop: two distinct item manifests, deduped");
		CHECK(hasDepot(w, 250900, 7988188911654056057ULL),
		      "workshop: installed manifest of item 1 (depotId==appId)");
		CHECK(hasDepot(w, 250900, 9070506685956686971ULL),
		      "workshop: installed manifest of item 2 (depotId==appId)");
	}

	// 11) extractWorkshopManifests: the LATEST manifest is warmed even when
	//     it differs from the currently-installed one (an update is pending,
	//     so Steam will plan the latest gid).
	{
		const std::string acf =
			"\"AppWorkshop\"\n{\n"
			"\t\"appid\"\t\t\"250900\"\n"
			"\t\"WorkshopItemsInstalled\"\n\t{\n"
			"\t\t\"111\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"1000\"\n\t\t}\n\t}\n"
			"\t\"WorkshopItemDetails\"\n\t{\n"
			"\t\t\"111\"\n\t\t{\n"
			"\t\t\t\"manifest\"\t\t\"1000\"\n"
			"\t\t\t\"latest_manifest\"\t\t\"2000\"\n\t\t}\n\t}\n}\n";
		auto w = Prewarm::extractWorkshopManifests(acf, 250900);
		CHECK(w.size() == 2, "workshop: installed + differing latest both warmed");
		CHECK(hasDepot(w, 250900, 1000ULL), "workshop: currently-installed gid");
		CHECK(hasDepot(w, 250900, 2000ULL), "workshop: pending latest gid");
	}

	// 12) extractWorkshopManifests: missing/empty/garbage ACF is safe, and a
	//     manifest "0" (no item) is ignored.
	{
		CHECK(Prewarm::extractWorkshopManifests("", 250900).empty(),
		      "workshop: empty ACF -> empty");
		CHECK(Prewarm::extractWorkshopManifests("no manifests here", 250900).empty(),
		      "workshop: garbage ACF -> empty");
		const std::string zero =
			"\"AppWorkshop\"\n{\n\t\"WorkshopItemsInstalled\"\n\t{\n"
			"\t\t\"1\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"0\"\n\t\t}\n\t}\n}\n";
		CHECK(Prewarm::extractWorkshopManifests(zero, 250900).empty(),
		      "workshop: manifest '0' ignored");
	}

	// 13) FailureTracker: a depot is blacklisted only after it fails to land
	//     on disk kMax times IN A ROW.  The pre-warm worker re-stages purged
	//     manifests every pass; a depot that stays inaccessible even after a
	//     fresh request-code (delisted / region-locked) must stop being
	//     retried — and re-logged — for the rest of the session, while a
	//     transient miss that later succeeds must NOT be permanently dropped.
	{
		Prewarm::FailureTracker ft(/*maxConsecutiveFailures=*/3);

		CHECK(!ft.isBlacklisted(100, 9), "tracker: unseen depot is not blacklisted");

		CHECK(!ft.recordFailure(100, 9), "tracker: 1st failure does not blacklist");
		CHECK(!ft.recordFailure(100, 9), "tracker: 2nd failure does not blacklist");
		CHECK(!ft.isBlacklisted(100, 9), "tracker: below threshold, still allowed");
		CHECK(ft.recordFailure(100, 9), "tracker: 3rd failure crosses threshold");
		CHECK(ft.isBlacklisted(100, 9), "tracker: blacklisted after kMax failures");
		ft.resetAll();
		CHECK(!ft.isBlacklisted(100, 9),
		      "tracker: a cooled-down pass re-admits the real manifest target");
	}

	// 14) FailureTracker: a success resets the consecutive-failure count, so a
	//     depot that recovers is never blacklisted by stale counts.
	{
		Prewarm::FailureTracker ft(3);
		ft.recordFailure(7, 1);
		ft.recordFailure(7, 1);
		ft.recordSuccess(7, 1);          // came back -> count cleared
		ft.recordFailure(7, 1);
		ft.recordFailure(7, 1);
		CHECK(!ft.isBlacklisted(7, 1),
		      "tracker: success resets the streak (2 fails after reset != 3)");
	}

	// 15) FailureTracker: distinct (depotId, gid) pairs are tracked
	//     independently — one bad depot doesn't blacklist its siblings.
	{
		Prewarm::FailureTracker ft(2);
		ft.recordFailure(10, 100);
		ft.recordFailure(10, 100);   // depot 10 now blacklisted
		CHECK(ft.isBlacklisted(10, 100), "tracker: depot 10 blacklisted");
		CHECK(!ft.isBlacklisted(11, 101), "tracker: sibling depot 11 unaffected");
		// Same depotId but a different gid is a different manifest.
		ft.recordFailure(10, 200);
		CHECK(!ft.isBlacklisted(10, 200),
		      "tracker: same depot, different gid tracked separately");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
