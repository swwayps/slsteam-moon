// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure manifest-scope predicate for the depot-key feature.
//
// SLSsteam's manifest machinery — the synchronous blob staging in
// CDepotDownloadMgr::BYldRequestDepotManifest and the GetManifestRequestCode
// injection in ManifestCode — must engage ONLY for content WE supply.  A
// depot key that Steam returned legitimately (an owned game, or a Proton
// Steam Linux Runtime) is cached for substitution but is NOT a trigger:
// engaging there fires a needless synchronous third-party fetch on a Steam
// worker thread and overwrites Steam's own request code, which is wasteful
// at best and a pipe-stall / SIGABRT vector at worst.
//
// The decision is kept here, free of globals and I/O, so it is unit-testable
// (tools/test_depotkey_scope.cpp).  The callers (manifestcode.cpp) supply the
// four signals from g_config / the key catalog.

#pragma once

namespace DepotKey
{
	// Should our manifest machinery engage for this (appId, depotId)?
	//
	//   appIsAddedApp     - the request's app id is an AdditionalApp
	//   depotIsAddedApp   - the depot id itself is an AdditionalApp
	//                       (workshop depots where depotId == appId)
	//   depotKeyIsManaged - we hold a key for this depot that WE injected
	//                       from a Lua script (NOT one merely observed from a
	//                       legitimate Steam response)
	//   depotHasPin       - an explicit manifest-gid pin exists for the depot
	inline bool depotInManifestScope(bool appIsAddedApp,
	                                 bool depotIsAddedApp,
	                                 bool depotKeyIsManaged,
	                                 bool depotHasPin)
	{
		return appIsAddedApp || depotIsAddedApp || depotKeyIsManaged || depotHasPin;
	}

	// The catalog's managed flag is sticky: once a depot is known to be
	// Lua-managed it stays managed even if Steam later returns the same key
	// in a legitimate response (which recvDepotKey caches as observed).
	// Used by saveKeyToCache when merging an incoming write with an existing
	// on-disk/in-memory entry, so a passive re-observation never downgrades a
	// LuaTools depot out of manifest scope.
	inline bool mergeManagedFlag(bool existingManaged, bool incomingManaged)
	{
		return existingManaged || incomingManaged;
	}
}
