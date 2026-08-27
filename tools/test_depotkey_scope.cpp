// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone test for the PURE manifest-scope predicate
// (src/feats/depotkey_scope.hpp :: depotInManifestScope).
//
// Why this exists
// ---------------
// SLSsteam's manifest machinery — synchronous blob staging in
// CDepotDownloadMgr::BYldRequestDepotManifest, plus GetManifestRequestCode
// injection — must engage ONLY for content WE provide: an AddedApp (by app
// or by depot), a depot whose decryption key WE injected from a Lua script,
// or a depot with an explicit manifest pin.
//
// The original scope check engaged whenever ANY depot key was cached.  But
// DepotKey::recvDepotKey passively caches EVERY key Steam returns with
// eresult=OK — including owned games and the Proton Steam Linux Runtimes
// (app 1070560/1391110, depots 1070561/1391111).  So installing/validating
// an owned title dragged its depots into scope: we fired a synchronous wudrm
// fetch on a Steam worker thread and overwrote Steam's legitimate manifest
// request code with our own — needless work and a stall/crash vector for
// content Steam fetches itself.  This pins down the corrected decision: a
// merely-observed key is NOT a trigger; only a *managed* (Lua-injected) key
// is.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_depotkey_scope.cpp -o /tmp/test_depotkey_scope && /tmp/test_depotkey_scope

#include "../src/feats/depotkey_scope.hpp"
#include "../src/feats/depotkey_import.hpp"
#include "../src/feats/depotkey_index.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using DepotKey::depotInManifestScope;

int main()
{
	// THE BUG: an owned game / Proton runtime whose depot key was merely
	// OBSERVED (cached by recvDepotKey) is NOT ours.  All flags false ->
	// out of scope, so Steam fetches its own manifest with no interference.
	CHECK(!depotInManifestScope(/*appIsAddedApp=*/false,
	                            /*depotIsAddedApp=*/false,
	                            /*depotKeyIsManaged=*/false,
	                            /*depotHasPin=*/false),
	      "observed-only depot (owned game / runtime) is out of scope");

	// An AddedApp identified by its app id -> in scope.
	CHECK(depotInManifestScope(true, false, false, false),
	      "AddedApp by app id is in scope");

	// A workshop-style depot whose depotId == an AddedApp id -> in scope.
	CHECK(depotInManifestScope(false, true, false, false),
	      "AddedApp by depot id is in scope");

	// The load-bearing case: a GetManifestRequestCode request can carry
	// app_id=0, so neither AddedApp check matches even for a LuaTools depot.
	// A key WE injected from the Lua script (managed) must still pull it in.
	CHECK(depotInManifestScope(false, false, true, false),
	      "Lua-managed depot key is in scope even when app_id is absent");

	// An explicit manifest pin -> in scope (pinning a specific gid).
	CHECK(depotInManifestScope(false, false, false, true),
	      "explicitly pinned depot is in scope");

	// Regression guard for the exact leak: even with an observed key on disk
	// (managed=false), absent every other signal, we stay out of scope.
	CHECK(!depotInManifestScope(false, false, false, false),
	      "observed key alone never forces scope (no downgrade of the fix)");

	// Any single positive signal suffices (independent OR terms).
	CHECK(depotInManifestScope(true, true, true, true),
	      "all signals present is in scope");

	// The managed flag is STICKY across catalog writes.  A depot first
	// imported from a Lua script (managed) must stay managed even when Steam
	// later returns the same key in a legitimate response (observed) — else
	// a LuaTools depot would silently drop out of scope mid-session.
	using DepotKey::mergeManagedFlag;
	CHECK(mergeManagedFlag(/*existing=*/false, /*incoming=*/false) == false,
	      "observed then observed stays unmanaged");
	CHECK(mergeManagedFlag(false, true) == true,
	      "observed then Lua-import upgrades to managed");
	CHECK(mergeManagedFlag(true, false) == true,
	      "Lua-managed then observed stays managed (no downgrade)");
	CHECK(mergeManagedFlag(true, true) == true,
	      "managed then managed stays managed");

	// The importer is called from both preinit and the later startup path.
	// A missing prerequisite must not consume the gate: a later startup call
	// with a valid Steam root must still run the importer.
	DepotKey::LuaScriptImportGate importGate;
	bool prerequisitesReady = false;
	int imported = 0;
	CHECK(!importGate.run([&] { return prerequisitesReady; },
	                      [&] { ++imported; }),
	      "missing Lua import prerequisite does not claim the gate");
	CHECK(!importGate.done(),
	      "missing Lua import prerequisite leaves the gate retryable");
	prerequisitesReady = true;
	CHECK(importGate.run([&] { return prerequisitesReady; },
	                     [&] { ++imported; }),
	      "Lua import claims the gate when prerequisites become available");
	CHECK(imported == 1,
	      "Lua import executes once after a retryable prerequisite miss");
	CHECK(!importGate.run([&] { return prerequisitesReady; },
	                      [&] { ++imported; }),
	      "completed Lua import rejects later claims");
	CHECK(imported == 1,
	      "completed Lua import remains idempotent");
	CHECK(importGate.rerun([&] { return prerequisitesReady; },
	                       [&] { ++imported; }),
	      "watcher rescan runs after the startup gate completed");
	CHECK(imported == 2,
	      "watcher rescan executes the importer exactly once per request");

	// A failed import must also leave the gate retryable, rather than
	// permanently suppressing the next startup/watcher attempt.
	DepotKey::LuaScriptImportGate failureGate;
	int attempts = 0;
	CHECK(!failureGate.run([] { return true; }, [&]
	{
		++attempts;
		throw std::runtime_error("deterministic import failure");
	}), "failed Lua import does not consume the gate");
	CHECK(!failureGate.done(), "failed Lua import remains retryable");
	CHECK(failureGate.run([] { return true; }, [&] { ++attempts; }),
	      "Lua import succeeds on the retry after a failure");
	CHECK(attempts == 2, "failed Lua import is attempted again exactly once");

	// Startup and watcher calls may race.  The synchronized gate permits only
	// one importer body even when all callers arrive concurrently.
	DepotKey::LuaScriptImportGate concurrentGate;
	std::atomic<int> concurrentImports{0};
	std::atomic<int> acceptedClaims{0};
	std::vector<std::thread> callers;
	for (int i = 0; i < 8; ++i)
	{
		callers.emplace_back([&]
		{
			if (concurrentGate.run([] { return true; },
			                         [&] { ++concurrentImports; }))
			{
				++acceptedClaims;
			}
		});
	}
	for (auto& caller : callers) caller.join();
	CHECK(concurrentImports.load() == 1,
	      "concurrent Lua import callers execute one body");
	CHECK(acceptedClaims.load() == 1,
	      "concurrent Lua import callers receive one claim");

	// The disk catalog is loaded lazily once, then all subsequent key writes
	// use the in-memory index.  This fixture makes the one-load contract
	// observable without linking the Steam protobuf hook implementation.
	DepotKey::LazyIndex<unsigned int, bool> index;
	int loadCalls = 0;
	index.loadOnce([&](auto& entries) {
		++loadCalls;
		entries.emplace(701u, false);
	});
	index.loadOnce([&](auto&) { ++loadCalls; });
	CHECK(loadCalls == 1, "depot-key index loader runs once");
	CHECK(index.find(701u) && !*index.find(701u),
	      "depot-key index keeps the parsed record");
	index.upsert(701u, DepotKey::mergeManagedFlag(*index.find(701u), true));
	index.upsert(701u, DepotKey::mergeManagedFlag(*index.find(701u), false));
	CHECK(index.find(701u) && *index.find(701u),
	      "depot-key index preserves managed after passive re-observation");

	DepotKey::ManagedDepotIndex appIndex;
	appIndex.replace(/*depotId=*/501u, /*oldAppId=*/0u, /*oldManaged=*/false,
	                 /*newAppId=*/100u, /*newManaged=*/true);
	appIndex.replace(/*depotId=*/502u, /*oldAppId=*/0u, /*oldManaged=*/false,
	                 /*newAppId=*/100u, /*newManaged=*/true);
	appIndex.replace(/*depotId=*/601u, /*oldAppId=*/0u, /*oldManaged=*/false,
	                 /*newAppId=*/200u, /*newManaged=*/true);
	CHECK(appIndex.forApp(100u) == std::vector<unsigned int>({501u, 502u}),
	      "app depot index returns only the managed depots for one app");
	CHECK(appIndex.forApp(999u).empty(),
	      "app depot index misses do not scan or leak another app's depots");
	appIndex.replace(/*depotId=*/502u, /*oldAppId=*/100u, /*oldManaged=*/true,
	                 /*newAppId=*/200u, /*newManaged=*/true);
	CHECK(appIndex.forApp(100u) == std::vector<unsigned int>({501u}) &&
	      appIndex.forApp(200u) == std::vector<unsigned int>({502u, 601u}),
	      "app depot index moves a rewritten depot between app ids");
	appIndex.replace(/*depotId=*/501u, /*oldAppId=*/100u, /*oldManaged=*/true,
	                 /*newAppId=*/100u, /*newManaged=*/false);
	CHECK(appIndex.forApp(100u).empty(),
	      "app depot index removes a depot that is no longer managed");

	if (g_failures == 0) std::printf("\nall depotkey-scope checks passed\n");
	else                 std::printf("\n%d depotkey-scope check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
