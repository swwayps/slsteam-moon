// SPDX-License-Identifier: AGPL-3.0-only
//
// Cold-start provisioner for AdditionalApps.
//
// Background
// ----------
// For an AdditionalApp that isn't in the user's library (no ticket / no
// AppToken), Valve's CM responds to PICS product-info with a stripped
// buffer that has no `depots` / `manifests` / `branches` blocks.  Steam's
// downloader then sees "0 depots" and the install dialog reports 0 B and
// finishes without downloading anything (visible in `content_log.txt`:
// `0 mounted depots`, `has no changes, 0 active: 0 target`).
//
// We previously tried to enrich the in-flight PICS buffer.  Steam
// validates the buffer against the SHA-1 it received in the PICS
// changelist, so any rewrite tripped the integrity check and looped the
// cold-cache login forever (see HANDOFF.md §3, FINDINGS.md).
//
// This module addresses that case at a different layer: it pulls a
// complete product-info text buffer from a public mirror and splices
// a synthetic entry into `appcache/appinfo.vdf` *before Steam opens the
// file*.  Steam then reads its own cache, finds the depots/manifests,
// and the downloader proceeds normally.  The runtime PICS path is left
// untouched, so the cold-loop fix is preserved.
//
// Provider: `https://api.steamcmd.net/v1/info/{appid}` (Pavel/SteamDB,
// JSON; identical schema to the SteamCMD `app_info_print`, including
// `_change_number` and `_sha`).  Selectable via
// `AppInfoProvision::setProvider` if a future config knob is wired up.
//
// Output writes through the same on-disk cache layout that
// `feats/pics.cpp` uses (`<config>/cache/picsbuffer_<appid>.bin` plus
// `picsbuffer_<appid>.yaml`).  The existing `AppInfoVdf::injectAllCached`
// then handles the splice at the next Steam start.
//
// Idempotent and best-effort: any network/parse failure is logged and
// skipped — Steam continues with whatever it has.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AppInfoProvision
{

// Collect the DLC appids advertised by every provisioned AdditionalApp,
// read from the on-disk `picsbuffer_<appid>.bin` buffers (their
// `extended.listofdlc` and `depots.<id>.dlcappid` fields).  Returns the
// deduplicated set, excluding the AddedApp base ids themselves.
//
// These ids must be injected into Steam's package-0 AppIdVec so the
// install planner schedules the DLC depots — ownership alone is not
// enough (proven on the VM 2026-06-05 with Binding of Isaac 250900).
// They are intentionally NOT added to g_config.addedAppIds, so they
// skip the per-app provisioning path (a DLC appid has no own depots and
// would only emit a "JSON has no depots" provisioning warning).
std::vector<uint32_t> collectDlcAppIdsForAddedApps();

// Fetch and persist a synthetic PICS buffer for `appId` if needed.
// `appinfoVdfPath` is the path to Steam's appcache/appinfo.vdf and is
// used to skip apps that already have a usable entry.  Returns true if
// a new buffer was written (or already cached).
bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath);

// Run `provisionApp` for every AdditionalApps id in the loaded config.
// Returns the number of buffers newly written (0 means everything was
// already provisioned or none needed).
int provisionAllAddedApps(const std::string& appinfoVdfPath);

// True iff `appId`'s appinfo depots were SYNTHESIZED from local manifests
// because its product-info is token-locked (access token denied -> empty
// PICS buffer).  Persisted across the setup() re-exec storm.  The outgoing
// PICS hook (apps.cpp) strips these from Steam's product-info request so a
// later empty refresh can't clobber the appinfo we spliced at startup
// (otherwise: install dialog -> 0 B / "Invalid install path").
bool isSynthesizedApp(uint32_t appId);

} // namespace AppInfoProvision
