// SPDX-License-Identifier: AGPL-3.0-only
//
// Background manifest pre-warm for AdditionalApps.
//
// Problem
// -------
// The install-time PICS recv handler (feats/pics.cpp) stages every
// depot-with-key SYNCHRONOUSLY, so the FIRST install of an AddedApp
// downloads first-attempt.  But Steam PURGES depotcache manifests that
// aren't part of the committed mount set, and a later planning pass that
// does NOT trigger a fresh PICS product-info request can't re-stage them:
//
//   * forcing a Proton compat tool on a native-Linux AddedApp re-plans
//     to the WINDOWS build, but its manifests were purged after the
//     native commit -> BYldRequestDepotManifest -> 'Access Denied' ->
//     one ~30s retry (proven on the Zorin VM 2026-06-05, BoI 250900);
//   * Steam re-validating files hits the same gap.
//
// Fix
// ---
// Keep every AddedApp's depot manifests (every OS we hold a key for, plus
// DLC) staged on disk in the BACKGROUND, re-staging to heal the post-
// commit purge, so any later planning pass finds them already present and
// skips BYld entirely.  The existing ManifestFetch (gid,depotId) dedup +
// on-disk re-check makes a re-stage cheap when the file is still there and
// a real re-fetch when Steam purged it.
//
// CRITICAL (HANDOFF DEAD END #2): the background worker must be started
// from a real Steam worker thread (the PICS InitFromPacket recv path),
// NEVER from the LD_AUDIT load()/setup() path — that crashed Steam twice.
//
// This header holds the PURE, dependency-free decision logic so it can be
// unit-tested without Steam, libcurl or disk (tools/test_prewarm.cpp).
// The threading / HTTP / disk runner lives in prewarm.cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Prewarm
{

// (depotId, public manifest gid).
using DepotGid = std::pair<uint32_t, uint64_t>;

// Tracks consecutive per-(depotId, gid) staging failures across pre-warm
// passes.  The worker re-stages purged manifests every ~30s; a depot that
// stays inaccessible even after a fresh request-code (delisted, region-
// locked, no longer on the CDN) would otherwise be retried — and warn-
// logged — forever.  After kMax consecutive failures the depot is dropped
// for the rest of the session; a single success clears the streak so a
// transient miss never permanently drops a depot.  Pure (no I/O), so the
// runner in prewarm.cpp stays a thin loop and this stays unit-testable.
class FailureTracker
{
public:
	explicit FailureTracker(int maxConsecutiveFailures)
	    : m_max(maxConsecutiveFailures) {}

	// The depot's manifest is on disk again: forget any failure streak.
	void recordSuccess(uint32_t depotId, uint64_t gid)
	{
		m_fails.erase(pack(depotId, gid));
	}

	// The depot's manifest is still missing after a staging attempt.
	// Returns true only on the attempt that crosses the threshold (so the
	// caller can log the blacklisting exactly once).
	bool recordFailure(uint32_t depotId, uint64_t gid)
	{
		const int n = ++m_fails[pack(depotId, gid)];
		return n == m_max;
	}

	bool isBlacklisted(uint32_t depotId, uint64_t gid) const
	{
		const auto it = m_fails.find(pack(depotId, gid));
		return it != m_fails.end() && it->second >= m_max;
	}

private:
	static uint64_t pack(uint32_t depotId, uint64_t gid)
	{
		return gid ^ (static_cast<uint64_t>(depotId) * 0x9E3779B97F4A7C15ULL);
	}

	int m_max;
	std::unordered_map<uint64_t, int> m_fails;
};

namespace detail
{

// One depot's relevant fields parsed from the wire buffer.
struct DepotInfo
{
	uint32_t depotId = 0;
	uint64_t gid = 0;        // manifests.public.gid (0 if none)
	std::string oslist;      // config.oslist ("" if none / shared)
};

// Single brace-depth walk over the appinfo `depots` block capturing, per
// depot, its public manifest gid AND its config.oslist.  This is the one
// place that understands the wire layout; both extractDepotsAndGids (gid
// only, used by the install staging path in feats/pics.cpp) and
// planStageTargets (gid + oslist) are thin views over it.
inline std::vector<DepotInfo> parseDepots(const std::string& buf)
{
	std::vector<DepotInfo> out;
	std::size_t depotsPos = buf.find("\"depots\"");
	if (depotsPos == std::string::npos) return out;

	std::size_t openBrace = buf.find('{', depotsPos + 8);
	if (openBrace == std::string::npos) return out;

	int depth = 1;
	std::size_t scan = openBrace + 1;
	uint32_t currentDepotId = 0;
	DepotInfo* current = nullptr;   // entry in `out` for currentDepotId
	std::string currentSection;
	std::string currentBranch;

	auto entryFor = [&out](uint32_t depotId) -> DepotInfo* {
		for (auto& e : out)
		{
			if (e.depotId == depotId) return &e;
		}
		out.push_back(DepotInfo{depotId, 0, {}});
		return &out.back();
	};

	while (scan < buf.size() && depth > 0)
	{
		char c = buf[scan];
		if (c == '"')
		{
			std::size_t end = buf.find('"', scan + 1);
			if (end == std::string::npos) break;
			std::string token = buf.substr(scan + 1, end - scan - 1);
			scan = end + 1;

			if (depth == 1)
			{
				bool isDigits = !token.empty();
				for (char ch : token)
				{
					if (ch < '0' || ch > '9') { isDigits = false; break; }
				}
				if (isDigits)
				{
					try
					{
						currentDepotId = std::stoul(token);
						current = entryFor(currentDepotId);
					}
					catch (...) { currentDepotId = 0; current = nullptr; }
				}
			}
			else if (depth == 2)
			{
				currentSection = token;
			}
			else if (depth == 3 && currentSection == "manifests")
			{
				currentBranch = token;
			}
			else if (depth == 3 && currentSection == "config" &&
			         token == "oslist" && current)
			{
				std::size_t valStart = buf.find('"', scan);
				if (valStart != std::string::npos)
				{
					std::size_t valEnd = buf.find('"', valStart + 1);
					if (valEnd != std::string::npos)
					{
						current->oslist =
						    buf.substr(valStart + 1, valEnd - valStart - 1);
						scan = valEnd + 1;
					}
				}
			}
			else if (depth == 4 && currentSection == "manifests" &&
			         currentBranch == "public" && token == "gid" &&
			         current)
			{
				std::size_t valStart = buf.find('"', scan);
				if (valStart != std::string::npos)
				{
					std::size_t valEnd = buf.find('"', valStart + 1);
					if (valEnd != std::string::npos)
					{
						std::string valToken =
						    buf.substr(valStart + 1, valEnd - valStart - 1);
						bool isDigits = !valToken.empty();
						for (char ch : valToken)
						{
							if (ch < '0' || ch > '9') { isDigits = false; break; }
						}
						if (isDigits)
						{
							try { current->gid = std::stoull(valToken); }
							catch (...) {}
						}
						scan = valEnd + 1;
					}
				}
			}
			continue;
		}
		if (c == '{') ++depth;
		else if (c == '}')
		{
			--depth;
			if (depth == 1)
			{
				currentDepotId = 0;
				current = nullptr;
			}
		}
		++scan;
	}
	return out;
}

// True if this oslist means "Steam on Linux will never request this depot"
// — i.e. the depot is macOS-only.  Linux runs native (linux) or the
// windows build under Proton, so linux and windows depots are BOTH wanted;
// only a depot whose ONLY platform is macOS is dead weight (warming it
// 401s the CDN and, looping, spams a critical popup — VM 2026-06-05).
// An empty oslist (no platform tag) is treated as wanted (shared depot).
inline bool isMacOnly(const std::string& oslist)
{
	if (oslist.empty()) return false;
	const bool hasMac = oslist.find("macos") != std::string::npos ||
	                    oslist.find("osx") != std::string::npos;
	if (!hasMac) return false;
	const bool hasLinux = oslist.find("linux") != std::string::npos;
	const bool hasWindows = oslist.find("windows") != std::string::npos ||
	                        oslist.find("win") != std::string::npos;
	return !hasLinux && !hasWindows;
}

} // namespace detail

// Parse a provisioned appinfo wire-text VDF buffer (the
// `picsbuffer_<appid>.bin` format) and return the `manifests.public.gid`
// of every depot it lists, regardless of OS or DLC status.  Depots with
// no `manifests.public.gid` (config-only / dlconly stubs) contribute
// nothing.  Shares one wire-layout parser (detail::parseDepots) with the
// pre-warm planner; feats/pics.cpp uses this view for install staging.
inline std::vector<DepotGid> extractDepotsAndGids(const std::string& buf)
{
	std::vector<DepotGid> results;
	for (const auto& d : detail::parseDepots(buf))
	{
		if (d.gid != 0)
		{
			results.push_back({d.depotId, d.gid});
		}
	}
	return results;
}


// Given the provisioned appinfo buffers of every AddedApp and a predicate
// telling us whether we hold a decryption key for a depot, return the
// deduplicated set of (depotId, gid) manifests worth keeping warm.
//
// We keep a depot only if BOTH:
//   * we hold its decryption key (`hasKey`) — staging a blob we can't
//     decrypt is a wasted CDN round-trip and the depot would never
//     install; and
//   * it is NOT macOS-only — Steam on Linux runs the native (linux) build
//     or the windows build under Proton, never macOS, so a macOS-only
//     depot's manifest 401s the CDN and, looped every pass, spams a
//     critical popup (observed on the VM 2026-06-05).
// Linux AND windows depots are both kept: the whole point is to have the
// windows depots ready before the user forces Proton, alongside native.
inline std::vector<DepotGid> planStageTargets(
    const std::vector<std::string>& provisionedBuffers,
    const std::function<bool(uint32_t depotId)>& hasKey)
{
	std::vector<DepotGid> out;
	std::unordered_set<uint64_t> seen;

	for (const std::string& buf : provisionedBuffers)
	{
		for (const auto& d : detail::parseDepots(buf))
		{
			if (d.gid == 0) continue;
			if (hasKey && !hasKey(d.depotId)) continue;
			if (detail::isMacOnly(d.oslist)) continue;

			// Pack (depotId, gid) into one key via a cheap mix so identical
			// depot/gid pairs across buffers dedup.  A collision only costs
			// a missed dedup (never a wrong stage), and the runtime layer
			// re-dedups by (gid,depotId) anyway.
			const uint64_t key =
			    d.gid ^ (static_cast<uint64_t>(d.depotId) * 0x9E3779B97F4A7C15ULL);
			if (seen.insert(key).second)
			{
				out.push_back({d.depotId, d.gid});
			}
		}
	}
	return out;
}

// Mine a workshop ACF (steamapps/workshop/appworkshop_<appid>.acf) for the
// manifest gids of subscribed workshop items, returning them as
// (appId, gid) pairs.  The workshop "depot" Steam plans has
// depotId == appId and a DYNAMIC per-item manifest gid that is NOT in the
// appinfo `depots` block (there, the appid only appears as the value of
// `workshopdepot`), so neither extractDepotsAndGids nor planStageTargets
// can see it.  The actual gids live in this ACF, in two blocks:
//   * WorkshopItemsInstalled.<itemId>.manifest    — what is installed now
//   * WorkshopItemDetails.<itemId>.latest_manifest — what is available
// We collect both (an update bumps latest_manifest before it installs, and
// Steam plans the latest gid), keyed by appId since that is the depotId
// Steam requests.  gid "0" (no item) is ignored.  Deduped.
//
// Pure string parsing so it is unit-testable without Steam or disk
// (tools/test_prewarm.cpp).  Robust to a missing/garbage ACF (returns {}).
inline std::vector<DepotGid> extractWorkshopManifests(const std::string& acf,
                                                      uint32_t appId)
{
	std::vector<DepotGid> out;
	std::unordered_set<uint64_t> seen;

	auto collectKey = [&](const std::string& key) {
		std::size_t pos = 0;
		const std::string needle = "\"" + key + "\"";
		while ((pos = acf.find(needle, pos)) != std::string::npos)
		{
			pos += needle.size();
			// Find the opening quote of the value that follows.
			std::size_t q1 = acf.find('"', pos);
			if (q1 == std::string::npos) break;
			std::size_t q2 = acf.find('"', q1 + 1);
			if (q2 == std::string::npos) break;
			const std::string val = acf.substr(q1 + 1, q2 - q1 - 1);
			pos = q2 + 1;

			bool isDigits = !val.empty();
			for (char ch : val)
			{
				if (ch < '0' || ch > '9') { isDigits = false; break; }
			}
			if (!isDigits) continue;

			uint64_t gid = 0;
			try { gid = std::stoull(val); }
			catch (...) { continue; }
			if (gid == 0) continue;

			if (seen.insert(gid).second)
			{
				out.push_back({appId, gid});
			}
		}
	};

	// Both the currently-installed manifest and the latest available one.
	collectKey("manifest");
	collectKey("latest_manifest");
	return out;
}


// --- Impure runtime API (implemented in prewarm.cpp) ----------------------

// Start the background pre-warm worker exactly once.  Idempotent and
// thread-safe: safe to call from every PICS recv.  MUST be called only
// from a real Steam worker thread (the PICS InitFromPacket recv path),
// NEVER from load()/setup() (HANDOFF DEAD END #2).  No-op if there are no
// AddedApps.  Returns immediately; the actual staging runs on its own
// detached thread.
void ensureStarted();

} // namespace Prewarm
