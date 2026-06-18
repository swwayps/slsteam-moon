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

#include "../sdk/protobufs/steammessages_clientserver_2.pb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

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

	// Importer for community-format Lua scripts.  Called at startup;
	// scans `config/stplug-in/*.lua` (under Steam) and ingests every
	// `addappid(depotId, 1, "<64 hex chars>")` into our cache, idempotently.
	void importLuaScripts();

	// Manifest provisioner: copies `<Steam>/config/depotcache/*.manifest` to
	// `<Steam>/depotcache/` so Steam's downloader can find them locally.
	void provisionManifests();

	// One-shot startup orchestration: importer + provisioner.
	void onStartup();

	// Hook callbacks — called from hooks.cpp inside the existing
	// CProtoBufMsgBase_Send / _InitFromPacket detours.
	void recvMsg(CProtoBufMsgBase* msg);
	void sendMsg(CProtoBufMsgBase* msg);

	// Specific protobuf-typed handlers.
	void recvDepotKey(CMsgClientGetDepotDecryptionKeyResponse* resp);
	void sendDepotKey(CMsgClientGetDepotDecryptionKey* req);
}
