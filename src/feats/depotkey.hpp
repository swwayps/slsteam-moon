// SPDX-License-Identifier: AGPL-3.0-only
//
// Depot decryption key feature.
//
// Intercepts CMsgClientGetDepotDecryptionKey requests/responses inside the
// existing CProtoBufMsgBase hooks (already provided by SLSsteam).
//
// Behaviour summary:
//   - Outgoing requests: track (depot_id -> app_id) so we can correlate the
//     incoming response (which only carries depot_id, not app_id).
//   - Incoming responses: if Steam returned a key, persist it; if Steam
//     returned an error AND we have a key in our local catalog, substitute
//     it so the downloader proceeds.
//   - On startup: import any community-format Lua scripts from
//     `<Steam>/config/stplug-in/*.lua` and ingest `addappid(d, 1, "hex")`
//     entries into our catalog.
//   - On startup: provision manifest files from `<Steam>/config/depotcache`
//     into `<Steam>/depotcache` so Steam's downloader skips the manifest
//     fetch step.
//
// Catalog layout (mirrors Ticket cache):
//   <SLSsteam config dir>/cache/depotkey_<depot_id>.yaml
//     ---
//     appId: <uint32>
//     depotId: <uint32>
//     key: <base64(32 bytes)>

#pragma once

#include "depotkey_recv_policy.hpp"

#include "../sdk/protobufs/steammessages_clientserver_2.pb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class CProtoBufMsgBase;

namespace DepotKey
{
	struct SavedKey
	{
		uint32_t appId  = 0;
		uint32_t depotId = 0;
		std::string key; // raw 32 bytes (binary, not hex)
		// True when WE supplied this key from a Lua script (a LuaTools
		// depot we manage); false when it was merely observed from a
		// legitimate Steam response (an owned game / Proton runtime).
		// Only managed keys put a depot into manifest scope.
		bool managed = false;
	};

	// Disk paths.
	std::string getKeyDir();
	std::string getKeyPath(uint32_t depotId);

	// Catalog management.
	SavedKey getCachedKey(uint32_t depotId);
	bool saveKeyToCache(uint32_t appId, uint32_t depotId, const std::string& key, bool managed);

	// True iff we hold a Lua-injected (managed) key for this depot — the
	// signal that SLSsteam's manifest machinery should engage for it.  A
	// merely-observed key (owned game / runtime) returns false.
	bool isManagedDepot(uint32_t depotId);

	// Central manifest-routing decision. Content Steam reports through a real
	// package stays on Steam's authenticated path, even when its Lua entry has
	// a manifest pin. Unlicensed managed content retains the local path.
	bool manifestInManagedScope(uint32_t appId, uint32_t depotId,
	                            bool depotHasPin = false,
	                            uint32_t contentAppId = 0);

	// Every MANAGED (Lua-injected) depot whose catalog entry records the
	// given appId.  The catalog is loaded lazily into an in-memory index and
	// parsed at most once per process.  Used to rebuild a token-locked app's
	// appinfo depots from data we already hold when its product-info comes
	// back without a `depots` block (manifestsynth).
	// Returns deduped depot ids; empty on any error / no $HOME.
	std::vector<uint32_t> managedDepotsForApp(uint32_t appId);

	// Importer for community-format Lua scripts.  Called at startup;
	// scans `config/stplug-in/*.lua` (under Steam) and ingests every
	// `addappid(depotId, 1, "<64 hex chars>")` into our cache, idempotently.
	void importLuaScripts();

	// Re-scan scripts after a watcher event. Unlike the startup one-shot this
	// runs once per request, under the same importer serialization lock.
	void reloadLuaScripts();

	// Manifest provisioner: copies `<Steam>/config/depotcache/*.manifest` to
	// `<Steam>/depotcache/` so Steam's downloader can find them locally.
	void provisionManifests();

	// One-shot startup orchestration: importer + provisioner.
	void onStartup();

	// Apply the config.vdf shader-cache setting during setup preinit, before
	// Steam's ConfigStore writers can publish a competing snapshot.
	void disableShaderCache();

	// Hook callbacks — called from hooks.cpp inside the existing
	// CProtoBufMsgBase_Send / _InitFromPacket detours.
	void recvMsg(CProtoBufMsgBase* msg);
	void sendMsg(CProtoBufMsgBase* msg);

	// RecvAction + classifyRecv (the pure substitution decision) live in
	// depotkey_recv_policy.hpp so they can be unit-tested without protobuf.

	// Specific protobuf-typed handlers. recvDepotKey returns true iff it
	// rewrote the response in place (so the CNetPacket CM-receive path
	// reserializes the packet); the classic CProtoBufMsgBase path ignores it.
	bool recvDepotKey(CMsgClientGetDepotDecryptionKeyResponse* resp);
	void sendDepotKey(CMsgClientGetDepotDecryptionKey* req);
}
