// SPDX-License-Identifier: AGPL-3.0-only
//
// Native anonymous Steam CM product-info client.
//
// Opens its OWN anonymous Steam CM websocket session (independent of the
// running Steam client's connection) and fetches PICS product-info for an
// arbitrary set of appids in ONE batched request.  This is the PRIMARY
// product-info source for AdditionalApps provisioning, replacing the
// external api.steamcmd.net mirror (which stays as a fallback in
// appinfo_provision.cpp).
//
// Product-info (depot list, manifest gids) is PUBLIC: any anonymous CM
// session can read it for any app.  The protected per-depot decryption
// key comes from the user's Lua, separately — so we need neither the
// user's account, ownership, nor any access token for these apps.
//
// Constraints:
//   - Uses its own socket/session; never touches the Steam client's CM
//     connection or any in-flight buffer (avoids the SHA-1 cold-loop).
//   - Fully SYNCHRONOUS, called from setup()'s provisioning pass on a
//     normal call stack — spawns NO threads from the LD_AUDIT preinit
//     path.
//   - Treats every CM byte as untrusted: bounds-checks all framing.
//   - Any failure (connect, logon, timeout, empty) returns false cleanly
//     so the caller falls back to steamcmd.net.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace CmClient
{

enum class FetchResult
{
	Success,
	NetworkUnavailable,
	Failed,
};

// Load libcurl while setup() is still on the safe preinit call stack. The
// first CM fetch may run from a Steam worker callback, where loading libcurl's
// TLS dependency tree can crash the 32-bit client.
bool prepareForThreadedFetch();

// Fetch PICS product-info wire buffers for `appids`: `out[appid]` receives
// the wire buffer for every app the CM returned one for, `changesOut` (when
// set) its change_number. A partial result is still Success, so the caller
// falls back only for the rest. NetworkUnavailable lets startup provisioning
// open its fleet-wide network circuit instead of entering per-app fallbacks.
FetchResult fetchProductInfoDetailed(
    const std::vector<uint32_t>& appids,
    std::unordered_map<uint32_t, std::string>& out,
    std::unordered_map<uint32_t, uint32_t>* changesOut = nullptr);


} // namespace CmClient
