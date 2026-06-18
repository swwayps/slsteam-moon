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
// is.  Verified live on the Zorin VM (runtime depot 1391111 leaked into
// scope before this fix).
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_depotkey_scope.cpp -o /tmp/test_depotkey_scope && /tmp/test_depotkey_scope

#include "../src/feats/depotkey_scope.hpp"

#include <cstdio>

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

	if (g_failures == 0) std::printf("\nall depotkey-scope checks passed\n");
	else                 std::printf("\n%d depotkey-scope check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
