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

// Detailed form used by startup provisioning so a directory/DNS failure can
// open its fleet-wide network circuit instead of entering per-app fallbacks.
FetchResult fetchProductInfoDetailed(
    const std::vector<uint32_t>& appids,
    std::unordered_map<uint32_t, std::string>& out,
    std::unordered_map<uint32_t, uint32_t>* changesOut = nullptr);

// Fetch PICS product-info wire buffers for `appids`.  On success fills
// `out[appid] = wire_text_vdf_buffer` for every app the CM returned a
// non-empty buffer for, and returns true.  On any error returns false
// (the caller should fall back to another provider).  Best-effort: a
// partial result (some apps present, some missing) still returns true so
// the caller can use what it got and fall back for the rest.
//
// When `changesOut` is non-null it also receives each returned app's
// `change_number` (used as part of the appinfo.vdf idempotency key).
bool fetchProductInfo(const std::vector<uint32_t>& appids,
                      std::unordered_map<uint32_t, std::string>& out,
                      std::unordered_map<uint32_t, uint32_t>* changesOut = nullptr);

} // namespace CmClient
