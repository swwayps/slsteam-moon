// SPDX-License-Identifier: AGPL-3.0-only
//
// See appinfo_provision.hpp for design notes.

#include "appinfo_provision.hpp"
#include "provision_result.hpp"

#include "appinfo_vdf.hpp"
#include "appinfostate.hpp"
#include "apps.hpp"
#include "cmclient.hpp"
#include "compattool.hpp"
#include "depotkey.hpp"
#include "emptydepot.hpp"
#include "dlcids.hpp"
#include "dlc_metadata.hpp"
#include "hotreload.hpp"
#include "kvtext.hpp"
#include "manifeststore.hpp"
#include "manifeststore_io.hpp"
#include "manifestsynth.hpp"
#include "provision_cache.hpp"
#include "cache_pair.hpp"
#include "provision_network.hpp"
#include "provision_schedule.hpp"
#include "pending_proton.hpp"
#include "provision_pass.hpp"
#include "provision_terminal.hpp"
#include "synthmark.hpp"
#include "usabledepot.hpp"

#include "../config.hpp"
#include "../ascii.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../sha1.hpp"
#include "../ownerwork.hpp"
#include "../bootprof.hpp"
#include "../thread_start.hpp"
#include "../cainfo.hpp"

#include "../utils/ManifestFetch.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/yaml.h"
#include "yaml-cpp/emitter.h"

#include "../utils/curl_api.hpp"
#include "../utils/curl_sink.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

namespace
{

// AdditionalApps that ended up windows-only after pruning native depots
// we have no key for.  These need a Proton CompatToolMapping so Steam
// will actually download + run the windows depot on Linux instead of
// treating the title as "no applicable platform" (which surfaces as the
// install committing 0 bytes / "0 mounted depots").
std::set<uint32_t> g_needProton;
// Config watcher removals can race a nonblocking cache lock. Keep a
// retryable in-memory tombstone so the next cache publication removes the
// corresponding pending Proton mapping instead of leaving stale state.
std::set<uint32_t> g_pendingProtonRemovals;

struct CacheValidationResult
{
	bool valid = false;
	std::string diag;
};

// The newest validation per app. A replaced pair gets a new identity and only
// the newest one can still match the files, so it takes the old entry's place
// instead of adding one for the rest of the session.
class CacheValidationMemo
{
public:
	const CacheValidationResult* find(const cache::CacheValidationKey& key) const
	{
		const auto found = entries_.find(key.appId);
		if (found == entries_.end() || !(found->second.first == key)) return nullptr;
		return &found->second.second;
	}
	void set(const cache::CacheValidationKey& key, CacheValidationResult result)
	{
		entries_.insert_or_assign(key.appId, std::make_pair(key, std::move(result)));
	}
	// The slot memoizeValidatedPublication assigns, emptied when the app's
	// entry held another identity.
	CacheValidationResult& operator[](const cache::CacheValidationKey& key)
	{
		auto& entry = entries_[key.appId];
		if (!(entry.first == key)) entry = {key, {}};
		return entry.second;
	}
	void erase(uint32_t appId) { entries_.erase(appId); }
private:
	std::unordered_map<uint32_t,
		std::pair<cache::CacheValidationKey, CacheValidationResult>> entries_;
};

std::mutex g_cacheValidationMu;
CacheValidationMemo g_cacheValidationMemo;

// DLC classification of depots, per base app, for one pair identity.
struct DepotDlcMemo
{
	cache::CacheValidationKey identity;
	std::unordered_map<uint32_t, uint32_t> dlcByDepot;
};
std::mutex g_depotDlcMemoMu;
std::unordered_map<uint32_t, DepotDlcMemo> g_depotDlcMemo;
std::unordered_map<uint32_t, CacheProbe> g_cacheProbeMemo;
std::mutex g_provisionPassMu;
std::mutex g_cachePublicationMu;
std::mutex g_cacheReadInvalidationMu;
std::unordered_set<uint32_t> g_cacheReadInvalidated;
std::unordered_map<uint32_t, std::uint64_t> g_cachePublicationGenerations;
// Apps whose last provisioning attempt ended in a result that can never
// publish a cache pair, keyed by the generation that produced it. Guarded by
// g_cachePublicationMu together with the generation map above.
struct TerminalMemoryEntry
{
	ProvisionTerminal::Record record;
	std::uint64_t managedGeneration = 0;
};
std::unordered_map<uint32_t, TerminalMemoryEntry> g_terminalProvisionResults;
std::unordered_set<uint32_t> g_loggedMalformedTerminal;
// Fingerprints are refreshed by startup/worker provisioning and the hot-reload
// coordinator.  Runtime PICS callbacks consult this memo only; they must not
// walk ManifestStore or read terminal sidecars while Steam is handling a
// product-info response.
std::unordered_map<uint32_t, std::string> g_localContentFingerprints;
std::mutex g_refreshScheduleMu;
RefreshQueue g_refreshQueue;
std::string g_refreshActivePath;
std::uint64_t g_refreshWorkerToken = 0;
std::mutex g_coldRetryMu;
struct ColdRetryState
{
	std::chrono::steady_clock::time_point retryAfter{};
	unsigned int failures = 0;
};
std::unordered_map<std::uint32_t, ColdRetryState> g_coldRetries;

void markProtonNeeded(uint32_t appId,
                      const CachePublicationToken& publication)
{
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	if (!publication.managed ||
	    !g_config.managedAppIds.contains(appId))
		return;

	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	if (!cache::protonPublicationAllowed(
	        publication.managed, publication.generation,
	        cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "AppInfoProvision: rejecting stale Proton mark for app=%u\n",
		    appId);
		return;
	}
	g_pendingProtonRemovals.erase(appId);
	g_needProton.insert(appId);
}

bool coldRetryBlocked(std::uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_coldRetryMu);
	const auto found = g_coldRetries.find(appId);
	return found != g_coldRetries.end() &&
	       std::chrono::steady_clock::now() < found->second.retryAfter;
}

void noteColdRetryOutcome(std::uint32_t appId, bool unresolved)
{
	std::lock_guard<std::mutex> lock(g_coldRetryMu);
	if (!unresolved)
	{
		g_coldRetries.erase(appId);
		return;
	}

	auto& retry = g_coldRetries[appId];
	++retry.failures;
	const unsigned int exponent =
		retry.failures > 4 ? 4 : retry.failures - 1;
	const unsigned int delaySeconds = std::min(60u, 5u << exponent);
	retry.retryAfter = std::chrono::steady_clock::now() +
	                   std::chrono::seconds(delaySeconds);
}





NetworkFailure httpGetJson(const std::string& url, std::string& body,
                           std::string& diag, long timeoutMs)
{
	if (!curlApi()) { diag = "libcurl unavailable"; return NetworkFailure::Provider; }
	if (timeoutMs <= 0)
	{
		diag = "startup network budget exhausted";
		return NetworkFailure::Transient;
	}
	CURL* c = curlApi()->easy_init();
	if (!c) { diag = "curl_easy_init failed"; return NetworkFailure::Provider; }

	body.clear();
	// A provider answer becomes a cache pair, which is at most 16 MiB.
	CurlBodySink sink{&body, 16u << 20};
	curlApi()->easy_setopt(c, CURLOPT_URL, url.c_str());
	curlApi()->easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curlApi()->easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, curlRedirectProtocols(url));
	curlApi()->easy_setopt(c, CURLOPT_WRITEFUNCTION, curlBodySinkWrite);
	curlApi()->easy_setopt(c, CURLOPT_WRITEDATA, &sink);
	// Bounded timeouts.  AppInfoProvision runs in setup() on the startup
	// path, so we must not stall Steam's launch for too long if
	// steamcmd.net is slow or unreachable.  The fetch is now wrapped in a
	// bounded retry (see provisionAppDetailed), and all provider attempts in this
	// startup pass share one 15-second wall-clock budget. A slow mirror can
	// therefore never multiply this timeout by app count or retry count.
	curlApi()->easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
	curlApi()->easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,
	                   std::min(timeoutMs, 8000L));
	// Same multi-thread safety justification as ManifestFetch::httpGet.
	curlApi()->easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curlApi()->easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-AppInfoProvision/0.1");
	// Pin the system trust store (see cainfo.hpp): the libcurl Steam loads
	// otherwise fails CA verification on SteamOS/Arch with curl error 60
	// ("Peer certificate cannot be authenticated"), the exact failure that
	// sinks the steamcmd.net fallback. No-op when no bundle is found.
	if (const char* f = ca::bundleFile()) curlApi()->easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  curlApi()->easy_setopt(c, CURLOPT_CAPATH, d);

	const CURLcode rc = curlApi()->easy_perform(c);
	long status = 0;
	curlApi()->easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curlApi()->easy_cleanup(c);

	if (rc != CURLE_OK)
	{
		diag = curlApi()->easy_strerror(rc);
		return classifyCurlFailure(rc);
	}
	if (status != 200)
	{
		std::stringstream s; s << "HTTP " << status;
		diag = s.str();
		return classifyHttpFailure(status);
	}
	if (body.empty()) { diag = "empty body"; return NetworkFailure::Provider; }
	return NetworkFailure::None;
}

// ---------------------------------------------------------------------------
// Provider list.  Each entry is a URL template with `{appid}`.
// First success wins.  Override via env SLSSTEAM_APPINFO_PROVIDER.
// ---------------------------------------------------------------------------

const std::vector<std::string>& providerChain()
{
	// Built once.  The first entry can be overridden via the env var
	// SLSSTEAM_APPINFO_PROVIDER (a URL template containing `{appid}`),
	// which is handy for testing the fetch/retry path against a custom
	// or deliberately-slow endpoint without rebuilding.
	static const std::vector<std::string> chain = [] {
		std::vector<std::string> c;
		if (const char* ov = std::getenv("SLSSTEAM_APPINFO_PROVIDER");
		    ov && *ov)
		{
			c.emplace_back(ov);
		}
		c.emplace_back("https://api.steamcmd.net/v1/info/{appid}");
		return c;
	}();
	return chain;
}

std::string expandUrl(const std::string& tmpl, uint32_t appId)
{
	std::string out;
	out.reserve(tmpl.size() + 16);
	const std::string needle = "{appid}";
	std::size_t i = 0;
	while (i < tmpl.size())
	{
		if (tmpl.compare(i, needle.size(), needle) == 0)
		{
			out += std::to_string(appId);
			i += needle.size();
		}
		else
		{
			out.push_back(tmpl[i++]);
		}
	}
	return out;
}

// A depot's config.oslist, or "" when it has none or it is unreadable.
std::string depotOsList(const YAML::Node& depot)
{
	try
	{
		if (depot.IsMap() && depot["config"] && depot["config"]["oslist"])
			return depot["config"]["oslist"].as<std::string>();
	}
	catch (...) {}
	return {};
}

} // namespace

// Strip depots that we have no decryption key for so Steam's
// downloader picks a depot we *can* actually decrypt.  External linkage only
// for tools/test_depot_prune.cpp.
//
// Background: api.steamcmd.net returns the full depot list (e.g. for
// dotAGE that's 638511=windows, 638512=macos, 638513=linux).  If the
// LuaTools plugin only provided a key for the windows depot, Steam on
// a Linux client picks the linux depot, finds no key, and drops to
// "0 mounted depots" — same observable bug as if appinfo were missing
// entirely.
//
// Behaviour:
//   - Drops every "<depot_id>" child of "depots" that lacks a cached
//     DepotKey, unless it's a DLC entry (`dlcappid` only) which carries
//     no payload to decrypt.
//   - Removes a DLC from extended.listofdlc when none of its content depots
//     is keyed.  Otherwise PackagePatch advertises the DLC as owned and
//     Steam independently plans the rejected encrypted content.
//   - Leaves "common.oslist" as upstream (see the NOTE at the end); the
//     surviving depots' OSes only decide whether the app needs Proton.
//
// Mutates `body` in place.  `appId` is for log lines only.
void pruneUnsupportedDepots(YAML::Node& body, uint32_t appId,
                             const CachePublicationToken& publication)
{
	if (!body.IsMap()) return;
	YAML::Node depots = body["depots"];
	if (!depots || !depots.IsMap()) return;

	// yaml-cpp's Node::remove() is unreliable when nodes are aliased
	// (which they are here — `body` was built by aliasing the parsed
	// tree).  Instead build a fresh depots map containing only the
	// entries we keep, then replace the whole block.  Cloning each
	// kept value via YAML::Clone breaks the alias so the rebuild is
	// self-contained.
	YAML::Node newDepots(YAML::NodeType::Map);
	std::set<std::string> survivingOs;
	bool linuxDepotDropped = false;
	int kept = 0;
	int keptContent = 0;
	int dropped = 0;
	int totalNumeric = 0;
	std::unordered_set<uint32_t> contentDlcAppIds;
	std::unordered_set<uint32_t> usableDlcAppIds;

	for (auto it = depots.begin(); it != depots.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();

		// Non-numeric keys are metadata (branches, baselanguages, …).
		// Always keep verbatim.
		bool numeric = !key.empty();
		for (char c : key) if (c < '0' || c > '9') { numeric = false; break; }
		if (!numeric)
		{
			newDepots[key] = YAML::Clone(it->second);
			continue;
		}
		++totalNumeric;

		uint32_t depotId = 0;
		try { depotId = static_cast<uint32_t>(std::stoul(key)); }
		catch (...) { newDepots[key] = YAML::Clone(it->second); continue; }

		const YAML::Node depotNode = it->second;
		const bool hasDlcMarker = depotNode.IsMap() && depotNode["dlcappid"];
		const bool hasManifests = depotNode.IsMap() && depotNode["manifests"];
		const bool isVirtualDlc = hasDlcMarker && !hasManifests;
		uint32_t dlcAppId = 0;
		if (hasDlcMarker)
		{
			try { dlcAppId = depotNode["dlcappid"].as<uint32_t>(); }
			catch (...) {}
		}

		// A DLC can own multiple depots.  Record every content-bearing
		// candidate now, before any drop path, and mark it usable only when at
		// least one of its entries survives.  This prevents one missing-key
		// sibling from hiding a valid keyed sibling.
		if (dlcAppId != 0 && hasManifests)
		{
			contentDlcAppIds.insert(dlcAppId);
		}

		// Drop empty (size-0) content depots.  Their manifest is a
		// degenerate stub — a single file mapping with an EMPTY name — and
		// Steam SEGV-crashes loading it during reconfigure
		// (Assert(!m_strName.IsEmpty()):contentmanifest.cpp:1630, seen live
		// on app=1868140 depot=4394810).  A size-0 depot has nothing to
		// install, so dropping it loses no content and keeps Steam from ever
		// planning the crash-inducing manifest.
		const bool hasKey = !DepotKey::getCachedKey(depotId).key.empty();
		if (depotPublicManifestIsEmpty(depotNode))
		{
			// Nothing to install, but a keyed DLC is still owned.
			if (dlcAppId != 0 && hasKey)
			{
				usableDlcAppIds.insert(dlcAppId);
			}
			++dropped;
			g_pLog->debug("AppInfoProvision: app=%u dropping empty depot %u "
			              "(public manifest size 0)\n", appId, depotId);
			continue;
		}

		// DLC entries have no `manifests` block; they're virtual and
		// don't need a decryption key — keep them.
		if (!hasKey && !isVirtualDlc)
		{
			++dropped;
			if (parsePlatformOsList(depotOsList(depotNode)).count("linux") != 0)
				linuxDepotDropped = true;
			continue;  // omit from newDepots
		}
		++kept;
		if (!isVirtualDlc) ++keptContent;
		newDepots[key] = YAML::Clone(depotNode);
		if (dlcAppId != 0)
		{
			usableDlcAppIds.insert(dlcAppId);
		}

		// Track which OSes the surviving depots support for the Proton
		// decision below; a depot without an oslist serves every platform.
		if (!isVirtualDlc && depotNode.IsMap())
			addDepotPlatforms(survivingOs, depotOsList(depotNode));
	}
	std::string commonOsList;
	try
	{
		if (body["common"] && body["common"]["oslist"])
			commonOsList = body["common"]["oslist"].as<std::string>();
	}
	catch (...) {}
	const bool protonNeeded = requiresProtonMapping(
		static_cast<std::size_t>(keptContent), survivingOs, commonOsList,
		linuxDepotDropped);

	// Keep PackagePatch's synthetic ownership list aligned with the depots
	// above.  A DLC is unsupported only when it had content entries and none
	// survived; list-only/virtual DLCs and DLCs with at least one keyed depot
	// remain untouched.
	std::unordered_set<uint32_t> unsupportedDlcAppIds;
	for (uint32_t dlcAppId : contentDlcAppIds)
	{
		if (usableDlcAppIds.count(dlcAppId) == 0)
		{
			unsupportedDlcAppIds.insert(dlcAppId);
		}
	}
	if (!unsupportedDlcAppIds.empty())
	{
		YAML::Node extended = body["extended"];
		YAML::Node listNode = extended && extended.IsMap()
			? extended["listofdlc"]
			: YAML::Node();
		if (listNode && listNode.IsScalar())
		{
			try
			{
				const std::string oldList = listNode.as<std::string>();
				std::size_t removed = 0;
				const std::string newList =
					filterUnsupportedDlcAppIds(oldList, unsupportedDlcAppIds,
					                           &removed);
				if (removed != 0)
				{
					body["extended"]["listofdlc"] = newList;
					g_pLog->info("AppInfoProvision: app=%u removed %zu unsupported "
					             "content DLC appid(s) from extended.listofdlc\n",
					             appId, removed);
				}
			}
			catch (...) {}
		}
	}

	if (dropped == 0)
	{
		// Even when we drop nothing, the app may be natively
		// non-Linux (e.g. the user added a windows-only title).  Mark
		// it for Proton when no surviving depot targets Linux.
		if (protonNeeded)
		{
			markProtonNeeded(appId, publication);
		}
		return;
	}

	body["depots"] = newDepots;
	g_pLog->debug("AppInfoProvision: app=%u dropped %d/%d unsupported depots (kept %d)\n",
	              appId, dropped, totalNumeric, kept);

	// If none of the surviving depots target Linux natively, the app
	// can only run through Proton.  Register it for a CompatToolMapping
	// so Steam downloads + runs the windows depot on Linux instead of
	// skipping it as "no applicable platform".  (Covers windows-only
	// and windows+macos apps.)
	if (protonNeeded)
	{
		markProtonNeeded(appId, publication);
	}

	// NOTE: we intentionally do NOT narrow common.oslist.  Leaving the
	// upstream oslist (e.g. "windows,macos,linux") intact while only
	// the windows depot survives matches what Steam itself stores for
	// Proton titles and lets the downloader pick the windows depot via
	// the CompatToolMapping.  Narrowing it to "windows" was observed to
	// leave the downloader stuck in "Reconfiguring" forever for
	// single-depot apps.
	(void)0;
}

namespace
{

// Forward declaration: defined with the on-disk cache helpers below.
const std::string& getCacheDir();
bool readValidatedCacheBufferLocked(uint32_t appId, std::string& wireOut,
                                    std::string& diag,
                                    bool* requiresProton = nullptr);

struct ProvisionPassContext
{
	std::unordered_map<uint32_t, std::string> cmBuffers;
	std::unordered_map<uint32_t, uint32_t> cmChanges;
	std::unordered_map<uint32_t, CachePublicationToken> cachePublications;
	// The newest archived gid per depot. The store is walked on first use and
	// the index then serves every fingerprint and depot rebuild of the pass.
	std::optional<ManifestStore::ArchivedGidIndex> archivedGids;

	const ManifestStore::ArchivedGidIndex& archived()
	{
		if (!archivedGids) archivedGids = ManifestStore::archivedGidIndex();
		return *archivedGids;
	}
};

// Rebuild a missing `depots` block for a token-locked app from data we
// already hold on disk.  Some titles (e.g. Risk of Rain 2, app 632360)
// have their PICS product-info gated behind an app access token Valve
// DENIES to anonymous sessions, so the anonymous-CM buffer (and the
// steamcmd fallback) come back with NO depots and provisioning would bail
// — leaving Steam at 0 B.  But the LuaTools zip already gave us the depot
// key (DepotKey cache) and the depot manifest (ManifestStore), so we can
// reconstruct the depots block ourselves: managed depots for this app,
// each pointing at its best archived manifest gid, with config.oslist
// inferred from the manifest so a Windows-only depot still gets the Proton
// CompatTool mapping downstream.  No-op when the body already has real
// depots (see ManifestSynth::injectSynthesizedDepots).  Mutates `body`;
// returns the number of depots synthesized.
int synthesizeDepotsFromStore(YAML::Node& body, uint32_t appId,
                              ProvisionPassContext& context)
{
	std::vector<ManifestSynth::SynthDepot> depots;
	// Accumulate file lists per OS across all of the app's depots so we can
	// synthesize one launch entry per platform (native + forced-Proton).
	std::vector<std::string> winFiles, linFiles, macFiles;
	for (uint32_t depotId : DepotKey::managedDepotsForApp(appId))
	{
		const auto& archived = context.archived();
		const auto found = archived.find(depotId);
		const uint64_t gid = found == archived.end() ? 0 : found->second;
		if (gid == 0) continue; // no archived manifest -> can't plan it

		// Read the archived manifest, derive the depot's OS from its parsed
		// file list (config.oslist, so pruneUnsupportedDepots mounts it on
		// the right platform / forces Proton only for genuinely windows-only
		// titles), and its total sizes (so the install dialog shows a real
		// size instead of "0 B").
		std::string oslist;
		uint64_t size = 0, download = 0;
		{
			const std::string man =
				ManifestStore::dir() + "/" + std::to_string(depotId) + "_" +
				std::to_string(gid) + ".manifest";
			std::ifstream ifs(man, std::ios::binary);
			if (ifs)
			{
				std::string bytes((std::istreambuf_iterator<char>(ifs)),
				                  std::istreambuf_iterator<char>());
				const auto files = ManifestSynth::extractManifestFilenames(bytes);
				oslist = ManifestSynth::detectOsFromFiles(files);
				if (oslist == "windows")     { for (auto& f : files) winFiles.push_back(f); }
				else if (oslist == "linux")  { for (auto& f : files) linFiles.push_back(f); }
				else if (oslist == "macos")  { for (auto& f : files) macFiles.push_back(f); }
				if (ManifestSynth::parseManifestSizes(bytes, size, download))
					ManifestStore::cacheInstalledSize(depotId, gid, size);
			}
		}
		depots.push_back({depotId, gid, oslist, size, download});
	}
	const int n = ManifestSynth::injectSynthesizedDepots(body, depots);
	if (n > 0)
	{
		// A token-locked app's product-info has no config block either, so
		// give Steam an installdir (derived from common.name) or it fails the
		// install with "Invalid install path".
		ManifestSynth::ensureInstallDir(body);

		// ...and no config.launch, so Steam refuses to start it ("Invalid
		// game configuration").  Synthesize one launch option per OS we hold
		// a depot for: the native one lets it run directly, the windows one
		// covers a forced Proton/compat tool.
		std::string installdir;
		if (YAML::Node d = body["config"]["installdir"]; d && d.IsScalar())
			installdir = d.as<std::string>();
		std::vector<std::pair<std::string, std::string>> launchers;
		if (!linFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(linFiles, installdir, "linux"), "linux"});
		if (!winFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(winFiles, installdir, "windows"), "windows"});
		if (!macFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(macFiles, installdir, "macos"), "macos"});
		const int le = ManifestSynth::ensureLaunchEntries(body, launchers);
		if (le > 0)
		{
			std::string summary;
			for (const auto& [exe, os] : launchers)
				if (!exe.empty()) summary += " " + os + ":'" + exe + "'";
			g_pLog->info("AppInfoProvision: app=%u synthesized %d launch entry(ies):%s\n",
			             appId, le, summary.c_str());
		}

		// The synthetic marker is published together with the validated cache
		// pair by persistBuffer. Keeping it out of this render phase prevents
		// stale work from recreating the marker after a managed-source removal.
	}
	return n;
}

// Render the SteamCMD-style JSON response for one app into the wire-text
// VDF format that AppInfoVdf::translateWireToIndexed accepts.
bool isDlcApp(const YAML::Node& body)
{
	if (!body || !body.IsMap()) return false;
	const YAML::Node common = body["common"];
	if (!common || !common.IsMap()) return false;
	const YAML::Node type = common["type"];
	if (!type || !type.IsScalar()) return false;
	std::string value = type.as<std::string>("");
	for (char& ch : value)
		ch = static_cast<char>(
			Ascii::toLower(static_cast<unsigned char>(ch)));
	return value == "dlc";
}

SourceResult renderAppinfoBuffer(
    const YAML::Node& appNode, uint32_t appId, std::string& wireOut,
    const CachePublicationToken& publication, ProvisionPassContext& context,
    bool* synthesizedOut)
{
	if (synthesizedOut) *synthesizedOut = false;
	if (!appNode.IsMap()) return SourceResult::InvalidResponse;

	// Drop SteamCMD synthetic envelope fields ("_change_number", "_sha",
	// "_size", "_missing_token").  They are not part of the appinfo
	// document Steam stores.
	YAML::Node body;
	for (auto it = appNode.begin(); it != appNode.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();
		if (!key.empty() && key.front() == '_') continue;
		body[key] = it->second;
	}
	if (!body.IsMap() || body.size() == 0) return SourceResult::InvalidResponse;

	// Remember whether the provider supplied any concrete content before
	// pruning. This still drives the IncompleteContent vs NoUsableContent split
	// (no depots at all vs depots rejected locally), but both are now terminal:
	// a well-formed response with no anonymous depot data is a stable property
	// of the app, so shouldTryProviderFallback() no longer retries it against
	// the steamcmd mirror (which reads the same anonymous product-info).
	bool hadConcreteContent = hasUsableContentDepot(body);

	// Token-locked apps (product-info access token denied to anonymous
	// sessions) arrive with NO depots.  Rebuild the block from the depot
	// key + archived manifest we already hold, so the prune/Proton/splice
	// tail below runs unchanged.  No-op when real depots are present.
	if (YAML::Node d = body["depots"]; !d || !d.IsMap() || d.size() == 0)
	{
		const int n = synthesizeDepotsFromStore(body, appId, context);
		if (n > 0)
		{
			if (synthesizedOut) *synthesizedOut = true;
			g_pLog->info("AppInfoProvision: app=%u synthesized %d depot(s) from "
			             "stored manifests (product-info had none)\n", appId, n);
			hadConcreteContent = hasUsableContentDepot(body);
		}
	}

	// Strip depots we can't decrypt; common.oslist stays as upstream.
	// Done here (post-envelope-strip, pre-emit) so the output Steam
	// reads is consistent and the change is invisible to Steam beyond
	// "the user only owns the windows depot".
	pruneUnsupportedDepots(body, appId, publication);
	const SourceResult contentResult = classifyContentResult(
	    hadConcreteContent, hasUsableContentDepot(body), isDlcApp(body));
	if (contentResult != SourceResult::Success)
	{
		g_pLog->info("AppInfoProvision: app=%u has no usable content depots "
		             "after pruning, skipping\n", appId);
		return contentResult;
	}

	// A locked app keeps the LIVE public gid in its provisioned appinfo so
	// Steam computes a genuine content delta; the pinned target is applied at
	// the post-commit reconcile instead, which avoids contaminating the
	// active/baseline manifest read.

	// Steam's appinfo wire format wraps the document in "appinfo" { ... }.
	wireOut = KvText::emitAppinfo(body);
	return SourceResult::Success;
}

// ---------------------------------------------------------------------------
// On-disk cache (mirrors feats/pics.cpp layout so AppInfoVdf::injectAllCached
// picks the buffers up at next start).
// ---------------------------------------------------------------------------

const std::string& getCacheDir()
{
	static const std::string dir = [] {
		const std::string value = g_config.getDir() + "/cache";
		std::error_code ec;
		if (!std::filesystem::exists(value, ec))
			std::filesystem::create_directories(value, ec);
		return value;
	}();
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	return getCacheDir() + "/picsbuffer_" + std::to_string(appId) + ".bin";
}

std::string getMetaPath(uint32_t appId)
{
	return getCacheDir() + "/picsbuffer_" + std::to_string(appId) + ".yaml";
}

std::string getDlcMetadataPath(uint32_t baseAppId)
{
	return getCacheDir() + "/dlcmetadata_" +
		std::to_string(baseAppId) + ".yaml";
}

bool readTextFileBounded(
	const std::string& path, std::size_t maxBytes, std::string& output)
{
	output.clear();
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input.is_open()) return false;
	const std::streamsize rawSize = input.tellg();
	if (rawSize <= 0 || static_cast<std::uint64_t>(rawSize) > maxBytes)
		return false;
	output.assign(static_cast<std::size_t>(rawSize), '\0');
	input.seekg(0, std::ios::beg);
	return input.read(output.data(), rawSize) && input.gcount() == rawSize;
}

std::string pendingProtonPath()
{
	return getCacheDir() + "/proton-mappings.pending";
}

enum class PendingProtonFileStatus
{
	Missing,
	Valid,
	Invalid,
};

struct PendingProtonFile
{
	PendingProtonFileStatus status = PendingProtonFileStatus::Invalid;
	std::set<uint32_t> ids;
};

// Read the pending mapping file while the caller holds cacheLock. Missing is
// a normal first-run state; every other open/read/parse failure is invalid and
// must leave the existing file and any in-memory removals untouched.
PendingProtonFile readPendingProtonFileLocked()
{
	const auto path = pendingProtonPath();
	std::ifstream ifs(path);
	if (!ifs.is_open())
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		if (!exists && !ec)
			return {PendingProtonFileStatus::Missing, {}};
		return {PendingProtonFileStatus::Invalid, {}};
	}

	std::set<uint32_t> ids;
	std::string token;
	while (ifs >> token)
	{
		const auto parsed = parsePendingProtonText(token);
		if (parsed.status != PendingProtonParseStatus::Valid ||
		    parsed.ids.size() != 1)
		{
			return {PendingProtonFileStatus::Invalid, {}};
		}
		ids.insert(*parsed.ids.begin());
	}
	if (ifs.bad() || !ifs.eof())
		return {PendingProtonFileStatus::Invalid, {}};
	return {PendingProtonFileStatus::Valid, std::move(ids)};
}

// Runtime PICS provisioning must not replace Steam's live config.vdf. Keep
// the ids in a small cache record for the next preinit pass instead.
void persistPendingProtonMappings(bool waitForLock)
{
	if (g_needProton.empty() && g_pendingProtonRemovals.empty()) return;
	(void)getCacheDir();
	ProcessLock::FileLock cacheLock(cacheLockPath(), !waitForLock);
	if (cacheLockHeldElsewhere(cacheLock)) return;

	const auto existing = readPendingProtonFileLocked();
	if (existing.status == PendingProtonFileStatus::Invalid)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: preserving invalid or unreadable pending Proton file\n");
		return;
	}

	std::set<uint32_t> pending;
	const auto managed = g_config.managedAppIds.get();
	for (uint32_t appId : existing.ids)
	{
		if (managed.count(appId) != 0)
			pending.insert(appId);
	}
	for (uint32_t appId : g_needProton)
	{
		if (managed.count(appId) != 0)
			pending.insert(appId);
	}
	for (uint32_t appId : g_pendingProtonRemovals)
		pending.erase(appId);

	bool persisted = false;
	std::string error;
	if (pending.empty())
	{
		std::error_code ec;
		std::filesystem::remove(pendingProtonPath(), ec);
		persisted = !ec;
	}
	else
	{
		std::string content;
		for (uint32_t appId : pending)
			content += std::to_string(appId) + "\n";
		persisted = AtomicFile::write(pendingProtonPath(), content, error);
	}
	if (!persisted)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: cannot persist pending Proton mappings: %s\n",
			              error.empty() ? "remove failed" : error.c_str());
		return;
	}
	g_pendingProtonRemovals.clear();
}

// Return the full identity of `appId`'s on-disk provisioned buffer, and
// whether it exists and is non-empty.  Used by provisionAppDetailed's short-lived
// cache to skip the network fetch during the setup() re-exec storm of a
// single boot and to key the expensive validation memo safely across atomic
// replacements.
bool statBuffer(uint32_t appId, cache::CacheValidationKey& identityOut)
{
	struct stat bufferStat{}, metadataStat{};
	if (stat(getBufferPath(appId).c_str(), &bufferStat) != 0 ||
		bufferStat.st_size <= 0 ||
		stat(getMetaPath(appId).c_str(), &metadataStat) != 0 ||
		metadataStat.st_size <= 0)
	{
		return false;
	}
	identityOut = cache::CacheValidationKey{
	    .appId = appId,
	    .mtimeSecs = static_cast<long long>(bufferStat.st_mtime),
	    .mtimeNsecs = static_cast<long long>(bufferStat.st_mtim.tv_nsec),
	    .size = static_cast<long long>(bufferStat.st_size),
	    .inode = static_cast<std::uint64_t>(bufferStat.st_ino),
	    .metadataMtimeSecs = static_cast<long long>(metadataStat.st_mtime),
	    .metadataMtimeNsecs = static_cast<long long>(metadataStat.st_mtim.tv_nsec),
	    .metadataSize = static_cast<long long>(metadataStat.st_size),
	    .metadataInode = static_cast<std::uint64_t>(metadataStat.st_ino),
	};
	return true;
}

std::int64_t cachePairMtimeSecsImpl(uint32_t appId) noexcept
{
	cache::CacheValidationKey identity{};
	if (!statBuffer(appId, identity)) return 0;
	return std::max<std::int64_t>(
		identity.mtimeSecs, identity.metadataMtimeSecs);
}

// Freshness window for legacy/full provider passes. Startup accepts a
// structurally validated pair regardless of age; PICS change numbers drive
// targeted refreshes without turning every relaunch into a catalog fetch.
// Override via SLSSTEAM_PROVISION_TTL (seconds; 0 disables the cache).
long long provisionTtlSecs()
{
	if (const char* ov = std::getenv("SLSSTEAM_PROVISION_TTL"); ov && *ov)
	{
		try { return std::stoll(ov); } catch (...) {}
	}
	return 300; // 5 minutes
}

bool persistBuffer(uint32_t appId, uint32_t changeNumber,
                   const std::string& sha20, const std::string& wire,
                   const CachePublicationToken& publication,
                   bool markSynthetic)
{
	(void)getCacheDir();
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock))
	{
		g_pLog->info("AppInfoProvision: unable to lock cache pair for app=%u\n", appId);
		return false;
	}
	if (!cache::cachePublicationAllowed(
	        publication.managed, publication.generation,
	        cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "AppInfoProvision: rejecting stale cache publication for app=%u\n",
		    appId);
		return false;
	}

	if (sha20.size() != 20)
	{
		g_pLog->debug("AppInfoProvision: refuse to persist app=%u, sha size %zu\n",
		              appId, sha20.size());
		return false;
	}
	YAML::Emitter em;
	em << YAML::BeginMap;
	em << YAML::Key << "appid"         << YAML::Value << appId;
	em << YAML::Key << "change_number" << YAML::Value << changeNumber;
	em << YAML::Key << "wire_size"     << YAML::Value << wire.size();
	em << YAML::Key << "sha_b64"       << YAML::Value << base64::to_base64(sha20);
	em << YAML::Key << "normalized"    << YAML::Value << true;
	em << YAML::Key << "synthetic"     << YAML::Value << markSynthetic;
	em << YAML::EndMap;
	const std::string metadata(em.c_str(), em.size());
	// The marker is published inside the same locked transaction as the pair,
	// so the generation gate above already covers it: nothing can change the
	// generation while this thread holds the publication mutex and cache lock.
	const bool markerBefore = SynthMark::isMarked(getCacheDir(), appId);
	// A refresh that renders the pair already on disk changes nothing:
	// writing it again would only cost backups and fsyncs and drop the base's
	// DLC metadata, which is still valid for these bytes.
	std::string onDisk;
	const bool unchanged = markerBefore == markSynthetic &&
		readTextFileBounded(getMetaPath(appId), metadata.size(), onDisk) &&
		onDisk == metadata &&
		readTextFileBounded(getBufferPath(appId), wire.size(), onDisk) &&
		onDisk == wire;
	std::string writeError;
	if (!unchanged &&
	    !publishCachePairLocked(appId, wire, metadata, markSynthetic,
	                            markerBefore, writeError))
	{
		g_pLog->debug(
		    "AppInfoProvision: unable to publish cache pair for app=%u: %s\n",
		    appId, writeError.c_str());
		return false;
	}
	ProvisionTerminal::Store(getCacheDir()).erase(appId);
	g_terminalProvisionResults.erase(appId);
	clearCacheReadInvalidation(appId);

	return memoizePublishedCachePairLocked(appId);
}

// ---------------------------------------------------------------------------
// Parse SteamCMD JSON response and extract envelope fields.  YAML-cpp
// reads JSON since JSON is a strict subset of YAML.
// ---------------------------------------------------------------------------

bool extractAppNode(const std::string& json, uint32_t appId,
                    YAML::Node& outApp, std::string& err)
{
	try
	{
		YAML::Node root = YAML::Load(json);
		if (!root.IsMap()) { err = "root not a map"; return false; }
		if (!root["data"]) { err = "no 'data'"; return false; }
		const auto data = root["data"];
		if (!data.IsMap()) { err = "data not a map"; return false; }
		const std::string key = std::to_string(appId);
		if (!data[key]) { err = "no entry for appid"; return false; }
		outApp = data[key];
		if (!outApp.IsMap()) { err = "app entry not a map"; return false; }
	}
	catch (const std::exception& e)
	{
		err = e.what();
		return false;
	}
	return true;
}

uint32_t pickChangeNumber(const YAML::Node& app)
{
	if (app["_change_number"])
	{
		try { return app["_change_number"].as<uint32_t>(); }
		catch (...) {}
	}
	return 0;
}

// (g_needProton declared near the top of this anonymous namespace.)

std::string steamRootForConfig()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	const std::vector<std::string> candidates = {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
	for (const auto& c : candidates)
	{
		struct stat st{};
		if (stat((c + "/steam.sh").c_str(), &st) == 0) return c;
	}
	return {};
}

// Build one index of depot ids backed by valid manifest artifacts.  The
// collector below may see many advertised DLC ids, so querying
// ManifestStore::bestArchivedGid() for each one would rescan the store once
// per id.  Index both durable storage locations once per collection pass;
// filenames are parsed with the same strict shape used by the DLC tests and
// invalid/corrupt files never qualify an id.
std::unordered_set<uint32_t> collectValidManifestDepotIds()
{
	std::unordered_set<uint32_t> depotIds;

	auto scanDirectory = [&depotIds](const std::filesystem::path& directory)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(directory, ec) || ec) return;

		std::filesystem::directory_iterator it(directory, ec);
		const std::filesystem::directory_iterator end;
		while (!ec && it != end)
		{
			const auto path = it->path();
			uint32_t depotId = 0;
			if (depotIdFromManifestName(path.filename().string(), depotId) &&
			    ManifestStoreIO::isValidManifest(path))
			{
				depotIds.insert(depotId);
			}
			it.increment(ec);
		}
	};

	scanDirectory(ManifestStore::dir());
	const auto root = steamRootForConfig();
	if (!root.empty())
	{
		scanDirectory(std::filesystem::path(root) / "depotcache");
	}
	return depotIds;
}

// Inject a `CompatToolMapping` entry for each app in g_needProton into
// `config/config.vdf`, so Steam runs them through Proton.  Best-effort,
// text-level edit (same approach as DepotKey::disableShaderCache).  Only
// adds entries that are missing; never overwrites a user's existing
// choice.  Each entry uses the user's default Steam Play tool (the "0" key
// of CompatToolMapping); Proton Experimental is only the fallback when no
// default is configured.
bool injectProtonMappings()
{
	if (g_needProton.empty()) return true;
	const auto root = steamRootForConfig();
	if (root.empty()) return false;
	const auto path = root + "/config/config.vdf";
	std::error_code existsError;
	if (!std::filesystem::exists(path, existsError)) return false;

	// This function is restricted to setup()'s preinit window. Runtime PICS
	// workers persist a pending set instead; Steam's ConfigStore writers do
	// not participate in our advisory lock, so a stat-then-rename check here
	// would still be a TOCTOU race against Steam.
	(void)getCacheDir();
	const auto configLockPath = cacheLockPath();
	ProcessLock::FileLock configLock(configLockPath, false);
	if (cacheLockHeldElsewhere(configLock))
	{
		g_pLog->debug("AppInfoProvision: config.vdf writer lock is busy\n");
		return false;
	}

	AtomicFile::FileIdentity expected{};
	if (!AtomicFile::readIdentity(path, expected)) return false;

	std::string content;
	{
		std::ifstream ifs(path);
		if (!ifs.is_open()) return false;
		std::stringstream ss; ss << ifs.rdbuf();
		content = ss.str();
	}

	// Locate (or create) the CompatToolMapping block under
	// InstallConfigStore/Software/Valve/Steam.
	std::size_t mapPos = content.find("\"CompatToolMapping\"");
	std::size_t mapBrace = std::string::npos;
	if (mapPos != std::string::npos)
	{
		mapBrace = content.find('{', mapPos);
	}
	else
	{
		// Insert a fresh CompatToolMapping block right after the
		// "Steam" object's opening brace.
		const auto steamPos = content.find("\"Steam\"");
		if (steamPos == std::string::npos) return false;
		const auto steamBrace = content.find('{', steamPos);
		if (steamBrace == std::string::npos) return false;
		const std::string block =
			"\n\t\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t\t{\n\t\t\t\t\t}";
		content.insert(steamBrace + 1, block);
		mapPos = content.find("\"CompatToolMapping\"");
		mapBrace = content.find('{', mapPos);
	}
	if (mapBrace == std::string::npos) return false;

	// Honour the user's default Steam Play compatibility tool (Settings ->
	// Compatibility -> Default compatibility tool), stored as the special
	// "0" key in this same block.  Fall back to Proton Experimental only
	// when the user has not chosen a default.
	std::string toolName = CompatTool::parseDefaultTool(content);
	if (toolName.empty()) toolName = "proton_experimental";

	int added = 0;
	for (uint32_t appId : g_needProton)
	{
		// An existing entry (the user's or ours) wins.
		if (CompatTool::hasAppMapping(content, appId))
		{
			continue;
		}
		const std::string key = "\"" + std::to_string(appId) + "\"";
		const std::string entry =
			"\n\t\t\t\t\t\t" + key + "\n\t\t\t\t\t\t{\n"
			"\t\t\t\t\t\t\t\"name\"\t\t\"" + toolName + "\"\n"
			"\t\t\t\t\t\t\t\"config\"\t\t\"\"\n"
			"\t\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
			"\t\t\t\t\t\t}";
		content.insert(mapBrace + 1, entry);
		++added;
	}

	if (added == 0) return true;

	std::string writeError;
	if (!AtomicFile::writeIfUnchanged(path, expected, content, writeError))
	{
		g_pLog->debug(
		    "AppInfoProvision: config.vdf changed before conditional publish: %s\n",
		    writeError.c_str());
		return false;
	}
	g_pLog->infoOnce("AppInfoProvision: injected %d Proton CompatToolMapping entr%s (tool=%s) into config.vdf\n",
	             added, added == 1 ? "y" : "ies", toolName.c_str());
	return true;
}

bool requiresProtonFromBody(const YAML::Node& body) noexcept
{
	try
	{
		const YAML::Node depots =
			body && body.IsMap() ? body["depots"] : YAML::Node();
		if (!depots || !depots.IsMap()) return false;

		std::size_t contentDepots = 0;
		std::set<std::string> depotOs;
		for (auto it = depots.begin(); it != depots.end(); ++it)
		{
			const std::string key = it->first.as<std::string>("");
			if (key.empty() || std::any_of(key.begin(), key.end(),
				[](char ch) { return ch < '0' || ch > '9'; })) continue;
			const YAML::Node depot = it->second;
			if (!depot || !depot.IsMap()) continue;
			const bool virtualDlc = depot["dlcappid"] && !depot["manifests"];
			if (virtualDlc) continue;
			++contentDepots;
			addDepotPlatforms(depotOs,
				depot["config"] && depot["config"]["oslist"]
					? depot["config"]["oslist"].as<std::string>("")
					: std::string());
		}

		std::string commonOsList;
		if (body["common"] && body["common"]["oslist"])
			commonOsList = body["common"]["oslist"].as<std::string>("");
		return requiresProtonMapping(contentDepots, depotOs, commonOsList);
	}
	catch (...)
	{
		return false;
	}
}
bool cachedWireRequiresProton(const std::string& wire) noexcept
{
	try
	{
		if (wire.empty()) return false;
		return requiresProtonFromBody(KvText::parseAppinfo(wire));
	}
	catch (...)
	{
		return false;
	}
}

bool readCacheMetadataFile(
	uint32_t appId,
	std::string& storage,
	cache::CacheMetadataView& metadata,
	std::string* diag = nullptr)
{
	const auto reject = [&](const char* message)
	{
		if (diag) *diag = message;
		return false;
	};

	std::ifstream input(getMetaPath(appId), std::ios::binary | std::ios::ate);
	if (!input.is_open()) return reject("metadata is missing");
	const std::streamsize rawSize = input.tellg();
	if (rawSize <= 0 || rawSize > (64LL << 10))
		return reject("metadata size is outside the accepted range");

	storage.assign(static_cast<std::size_t>(rawSize), '\0');
	input.seekg(0, std::ios::beg);
	if (!input.read(storage.data(), rawSize) || input.gcount() != rawSize)
		return reject("metadata read failed");
	if (!cache::parseCacheMetadata(storage, metadata))
		return reject("metadata format is invalid");
	return true;
}

bool cacheMetadataMarkerAllowsRead(
	uint32_t appId,
	const cache::CacheMetadataView& metadata)
{
	{
		std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
		if (g_cacheReadInvalidated.count(appId) != 0)
			return false;
	}
	const bool markerPresent = SynthMark::isMarked(getCacheDir(), appId);
	return cache::syntheticMarkerStateConsistent(
		metadata.hasSynthetic, metadata.synthetic, markerPresent);
}

bool readValidatedCacheBufferLocked(uint32_t appId, std::string& wireOut,
                                    std::string& diag, bool* requiresProton)
{
	try
	{
		std::string metadataText;
		cache::CacheMetadataView metadata;
		if (!readCacheMetadataFile(
				appId, metadataText, metadata, &diag))
		{
			return false;
		}
		if (!cacheMetadataMarkerAllowsRead(appId, metadata))
		{
			diag = "synthetic marker state is inconsistent";
			return false;
		}
		const std::string declaredSha = std::string(
		    base64::from_base64(std::string(metadata.shaBase64)));

		std::ifstream ifs(getBufferPath(appId), std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) { diag = "buffer is missing"; return false; }
		const std::streamsize rawSize = ifs.tellg();
		if (rawSize <= 0 || rawSize > (16LL << 20))
		{
			diag = "buffer size is outside the accepted range";
			return false;
		}
		std::string wire(static_cast<size_t>(rawSize), '\0');
		ifs.seekg(0, std::ios::beg);
		if (!ifs.read(wire.data(), rawSize))
		{
			diag = "buffer read failed";
			return false;
		}

		// Callers hold the cache lock, so a pair identity validated earlier in
		// this process still names these bytes: skip the hash and the parse.
		cache::CacheValidationKey identity{};
		const bool identified = statBuffer(appId, identity) &&
			identity.size == static_cast<long long>(wire.size());
		if (identified)
		{
			std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
			const auto* found = g_cacheValidationMemo.find(identity);
			if (found != nullptr && found->valid)
			{
				if (requiresProton) *requiresProton = cachedWireRequiresProton(wire);
				wireOut = std::move(wire);
				return true;
			}
		}

		std::uint8_t digestBytes[20]{};
		Sha1::digest(wire.data(), wire.size(), digestBytes);
		const std::string actualSha(
		    reinterpret_cast<const char*>(digestBytes), sizeof(digestBytes));

		const YAML::Node appNode = KvText::parseAppinfo(wire);
		const bool parsed = appNode && appNode.IsMap() && appNode.size() > 0;
		const cache::CacheRecordFacts facts{
		    .requestedAppId = appId,
		    .metadataAppId = metadata.appId,
		    .declaredSize = metadata.wireSize,
		    .actualSize = wire.size(),
		    .shaSize = declaredSha.size(),
		    .shaMatches = declaredSha == actualSha,
		    .parsed = parsed,
		    .hasUsableContent = parsed && hasUsableContentDepot(appNode),
		};
		if (!cache::isCacheRecordValid(facts))
		{
			diag = "metadata, SHA-1, or depot validation failed";
			return false;
		}
		if (identified)
		{
			std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
			g_cacheValidationMemo.set(identity, CacheValidationResult{true, {}});
		}
		if (requiresProton) *requiresProton = requiresProtonFromBody(appNode);
		wireOut = std::move(wire);
		return true;
	}
	catch (const std::exception& e)
	{
		diag = e.what();
		return false;
	}
}

bool hasValidatedCachedBuffer(uint32_t appId, std::string& diag)
{
	std::string wire;
	return readValidatedCacheBufferLocked(appId, wire, diag);
}

bool cachedWireSizeMatches(uint32_t appId, long long actualSize)
{
	if (actualSize <= 0) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	return readCacheMetadataFile(appId, metadataText, metadata) &&
		cache::wireSizeMatches(
			static_cast<unsigned long long>(actualSize), metadata.wireSize);
}

cache::CacheUse cacheUseForApp(uint32_t appId, bool refreshUnavailable)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return cache::CacheUse::None;

	cache::CacheValidationKey key{};
	const bool present = statBuffer(appId, key);
	if (!present) return cache::CacheUse::None;

	const long long now = static_cast<long long>(std::time(nullptr));
	const bool fresh = cache::isBufferReusable(
	    present, key.mtimeSecs, now, provisionTtlSecs());
	if (!cache::shouldValidateCache(fresh, refreshUnavailable))
	{
		// A stale online buffer will be refreshed; do not parse YAML, hash the
		// whole wire or build a VDF tree just to discard it below.
		return cache::CacheUse::None;
	}

	// Cheap metadata gate before the expensive integrity/structure check.
	if (!cachedWireSizeMatches(appId, key.size))
	{
		g_pLog->info("AppInfoProvision: app=%u cached buffer rejected (wire_size mismatch)\n",
		             appId);
		return cache::CacheUse::None;
	}

	CacheValidationResult result;
	bool memoHit = false;
	{
		std::lock_guard<std::mutex> lk(g_cacheValidationMu);
		if (const auto* found = g_cacheValidationMemo.find(key))
		{
			result = *found;
			memoHit = true;
		}
	}
	if (!memoHit)
	{
		result.valid = hasValidatedCachedBuffer(appId, result.diag);
		std::lock_guard<std::mutex> lk(g_cacheValidationMu);
		g_cacheValidationMemo.set(key, result);
	}
	if (result.valid && !cacheMarkerAllowsRead(appId))
	{
		result.valid = false;
		result.diag = "synthetic marker is missing";
	}
	if (!result.valid)
	{
		g_pLog->info("AppInfoProvision: app=%u cached buffer rejected (%s)\n",
		             appId, result.diag.c_str());
	}
	return cache::chooseCacheUse(result.valid, fresh, refreshUnavailable);
}

// Classify what is on disk for one app, separating "unusable" from "usable but
// past the freshness window" so each caller can apply its own policy.
CacheProbe probeCacheImpl(uint32_t appId, CacheProbeMode mode)
{
	CacheProbe probe;
	if (mode == CacheProbeMode::NonBlockingMemoOnly)
	{
		std::unique_lock<std::mutex> validationLock(
			g_cacheValidationMu, std::try_to_lock);
		if (!validationLock.owns_lock())
		{
			probe.readiness = CacheReadiness::Busy;
			return probe;
		}
		const auto found = g_cacheProbeMemo.find(appId);
		if (found == g_cacheProbeMemo.end())
			probe.readiness = CacheReadiness::Unverified;
		else
			probe = found->second;
		return probe;
	}

	const auto remember = [appId](CacheProbe result) {
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		g_cacheProbeMemo[appId] = result;
		return result;
	};
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock))
	{
		probe.readiness = CacheReadiness::Invalid;
		return remember(probe);
	}

	struct stat bufferStat{}, metadataStat{};
	const bool bufferExists = stat(getBufferPath(appId).c_str(), &bufferStat) == 0 &&
		bufferStat.st_size > 0;
	const bool metadataExists = stat(getMetaPath(appId).c_str(), &metadataStat) == 0 &&
		metadataStat.st_size > 0;
	if (!bufferExists && !metadataExists) return remember(probe);
	if (!bufferExists || !metadataExists)
	{
		probe.readiness = CacheReadiness::Invalid;
		return remember(probe);
	}

	cache::CacheValidationKey key{};
	if (!statBuffer(appId, key))
	{
		probe.readiness = CacheReadiness::Invalid;
		return remember(probe);
	}
	std::string metadataText;
	cache::CacheMetadataView metadata;
	if (!readCacheMetadataFile(appId, metadataText, metadata) ||
		metadata.appId != appId || metadata.wireSize !=
			static_cast<std::uint64_t>(key.size))
	{
		probe.readiness = CacheReadiness::Invalid;
		return remember(probe);
	}
	probe.changeNumber = metadata.changeNumber;

	CacheValidationResult validation;
	bool memoHit = false;
	{
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		if (const auto* found = g_cacheValidationMemo.find(key))
		{
			validation = *found;
			memoHit = true;
		}
	}
	if (!memoHit)
	{
		validation.valid = hasValidatedCachedBuffer(appId, validation.diag);
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		g_cacheValidationMemo.set(key, validation);
	}
	if (validation.valid && !cacheMarkerAllowsRead(appId))
		validation.valid = false;
	if (!validation.valid)
	{
		probe.readiness = CacheReadiness::Invalid;
		return remember(probe);
	}
	const long long now = static_cast<long long>(std::time(nullptr));
	probe.readiness = cache::isBufferReusable(
		true, key.mtimeSecs, now, provisionTtlSecs())
		? CacheReadiness::Fresh : CacheReadiness::ValidStale;
	return remember(probe);
}

// Render+prune+sha+persist a parsed appinfo node (shared tail used by
// both the CM and steamcmd paths). `changeNumber` is the PICS/JSON change
// number for the meta record.
SourceResult renderAndPersist(uint32_t appId, const YAML::Node& appNode,
                              uint32_t changeNumber,
                              const CachePublicationToken& publication,
                              ProvisionPassContext& context)
{
	std::string wire;
	bool synthesized = false;
	const SourceResult renderResult = renderAppinfoBuffer(
	    appNode, appId, wire, publication, context, &synthesized);
	if (renderResult != SourceResult::Success)
	{
		const char* reason = "invalid response";
		if (renderResult == SourceResult::IncompleteContent)
			reason = "response contains no concrete depot data";
		else if (renderResult == SourceResult::NoUsableContent)
			reason = "concrete depots are not usable";
		else if (renderResult == SourceResult::VirtualDlc)
			reason = "DLC has no usable content depots";
		g_pLog->debug("AppInfoProvision: app=%u render stopped (%s)\n", appId, reason);
		return renderResult;
	}

	// Hash our own bytes, never the provider's `_sha`: the buffer drops or
	// synthesizes depots, and AppInfoVdf::injectApp skips the rewrite when
	// (appid, change number, sha) match the entry already on disk.
	std::string sha20;
	{
		std::uint8_t tmp[20];
		Sha1::digest(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	if (!persistBuffer(appId, changeNumber, sha20, wire, publication,
	                   synthesized))
	{
		g_pLog->info("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return SourceResult::LocalFailure;
	}

	g_pLog->debugOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	                  appId, changeNumber, wire.size());
	return SourceResult::Success;
}

// Provision one app from a native-CM wire buffer. Mirrors the steamcmd
// path's tail but skips the JSON parse — the CM buffer is already wire VDF.
SourceResult provisionAppFromCmBuffer(uint32_t appId, const std::string& cmWire,
                                      uint32_t changeNumber,
                                      const CachePublicationToken& publication,
                                      ProvisionPassContext& context)
{
	if (cmWire.empty()) return SourceResult::InvalidResponse;
	YAML::Node appNode = KvText::parseAppinfo(cmWire);
	if (!appNode || !appNode.IsMap() || appNode.size() == 0)
	{
		g_pLog->info("AppInfoProvision: app=%u CM buffer parse failed, fallback\n", appId);
		return SourceResult::InvalidResponse;
	}
	return renderAndPersist(appId, appNode, changeNumber, publication, context);
}

struct PendingProtonLoad
{
	bool lockAcquired = false;
	PendingProtonFileStatus status = PendingProtonFileStatus::Invalid;
	std::set<uint32_t> fileIds;
	std::set<uint32_t> ids;
};

PendingProtonLoad loadPendingProtonMappings()
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return {};

	const auto file = readPendingProtonFileLocked();
	PendingProtonLoad result;
	result.lockAcquired = true;
	result.status = file.status;
	result.fileIds = file.ids;
	if (file.status != PendingProtonFileStatus::Valid) return result;

	const auto managed = g_config.managedAppIds.get();
	for (uint32_t appId : file.ids)
	{
		if (managed.count(appId) != 0)
			result.ids.insert(appId);
	}
	return result;
}

bool removePendingProtonMappingLocked(uint32_t appId)
{
	const auto file = readPendingProtonFileLocked();
	if (file.status == PendingProtonFileStatus::Missing) return true;
	if (file.status != PendingProtonFileStatus::Valid)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: preserving invalid or unreadable pending Proton file while removing app=%u\n",
		              appId);
		return false;
	}

	std::set<uint32_t> pending = file.ids;
	if (pending.erase(appId) == 0) return true;

	if (pending.empty())
	{
		std::error_code ec;
		std::filesystem::remove(pendingProtonPath(), ec);
		return !ec;
	}

	std::string content;
	for (uint32_t pendingAppId : pending)
		content += std::to_string(pendingAppId) + "\n";
	std::string error;
	const bool removed = AtomicFile::write(pendingProtonPath(), content, error);
	if (!removed && g_pLog)
	{
		g_pLog->debug("AppInfoProvision: cannot remove pending Proton mapping for app=%u: %s\n",
		              appId, error.c_str());
	}
	return removed;
}

// Remove a successfully applied record only if no writer changed it after the
// initial locked read. This closes the gap between load/apply and cleanup
// without holding the cache lock across the config.vdf rewrite.
bool clearPendingProtonMappingsIfUnchanged(
    const std::set<uint32_t>& expectedFileIds)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return false;

	const auto current = readPendingProtonFileLocked();
	if (current.status != PendingProtonFileStatus::Valid ||
	    current.ids != expectedFileIds)
	{
		if (g_pLog)
			g_pLog->debug(
			    "AppInfoProvision: pending Proton file changed during flush; preserving it\n");
		return false;
	}

	std::error_code ec;
	std::filesystem::remove(pendingProtonPath(), ec);
	return !ec;
}

bool isSynthesizedAppLocked(uint32_t appId)
{
	if (appId == 0) return false;
	const bool active = g_config.isAddedAppId(appId);
	bool invalidated = false;
	{
		std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
		invalidated = g_cacheReadInvalidated.count(appId) != 0;
	}
	// Managed-source removal invalidates cache reads but may retain an active
	// compatibility app. Keep the live appinfo protected while its marker is
	// preserved; a later full removal clears both ownership and protection.
	if (invalidated && !active) return false;
	const bool markerPresent = SynthMark::isMarked(getCacheDir(), appId);
	if (!markerPresent) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	if (readCacheMetadataFile(appId, metadataText, metadata))
	{
		if (metadata.hasSynthetic)
			return metadata.synthetic;
		// Before explicit provenance metadata existed, the persisted marker
		// itself was the synthetic bit. Preserve that behavior for existing
		// installations instead of requiring migration on first boot.
		return true;
	}

	// Managed-source cleanup deliberately retains the marker after moving
	// the cache pair. An active compatibility app must remain protected by
	// that marker until a new publication or full removal reconciles it.
	std::error_code metadataError;
	const bool metadataPresent = std::filesystem::exists(
	    getMetaPath(appId), metadataError);
	return !metadataError &&
	       cache::retainedSyntheticMarkerProtectionAllowed(
	           markerPresent, metadataPresent, active);
}

} // namespace

bool requiresProton(std::uint32_t appId) noexcept
{
	try
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		return appId != 0 && g_needProton.count(appId) != 0;
	}
	catch (...)
	{
		return false;
	}
}

CacheProbe probeCache(uint32_t appId, CacheProbeMode mode)
{
	return probeCacheImpl(appId, mode);
}

std::int64_t cachePairMtimeSecs(uint32_t appId) noexcept
{
	return cachePairMtimeSecsImpl(appId);
}

std::int64_t dlcMetadataCacheMtimeSecs(uint32_t appId) noexcept
{
	struct stat st{};
	if (appId == 0 || stat(getDlcMetadataPath(appId).c_str(), &st) != 0)
		return 0;
	return static_cast<std::int64_t>(st.st_mtime);
}

void sha1Bytes(const void* data, std::size_t size, std::uint8_t out[20])
{
	Sha1::digest(data, size, out);
}

bool publishCachePairLocked(uint32_t appId, const std::string& wire,
                            const std::string& metadata, bool synthetic,
                            bool markerBefore, std::string& error)
{
	const auto cacheDir = getCacheDir();
	// Invalidate terminal state before replacing the pair. If the pair write
	// fails, the old terminal verdict is gone and the app remains retryable;
	// retaining it would allow a stale sidecar to suppress the next refresh.
	g_terminalProvisionResults.erase(appId);
	if (!ProvisionTerminal::Store(cacheDir).erase(appId))
	{
		error = "unable to invalidate terminal sidecar";
		return false;
	}
	const bool published = CachePair::publish(
		getBufferPath(appId), getMetaPath(appId), wire, metadata, synthetic,
		markerBefore,
		[&](bool desired) {
			return desired ? SynthMark::mark(cacheDir, appId)
			               : SynthMark::unmark(cacheDir, appId);
		},
		[&] { return SynthMark::isMarked(cacheDir, appId); }, error);
	if (!published) return false;

	// The pair transaction may restore the previous base on failure, so its
	// compatible child metadata must survive that rollback. Invalidate only
	// after the new pair commits. Readers share the cache lock held by the
	// caller and cannot observe the new base beside the old sidecar; even after
	// an interrupted cleanup, the sidecar's base digest makes it unusable.
	if (cache::shouldInvalidateDlcMetadata(published))
	{
		std::error_code metadataError;
		std::filesystem::remove(getDlcMetadataPath(appId), metadataError);
		if (metadataError)
		{
			// The pair is already committed and cannot be reported as rolled
			// back. The old sidecar remains fail-closed: readers compare its
			// base SHA/change to the new pair and schedule a later repair.
			g_pLog->debug(
				"AppInfoProvision: cache pair committed for app=%u but DLC "
				"metadata cleanup failed: %s\n",
				appId, metadataError.message().c_str());
		}
	}
	return true;
}

bool memoizePublishedCachePairLocked(uint32_t appId)
{
	cache::CacheValidationKey publishedKey{};
	const bool identityCaptured = statBuffer(appId, publishedKey);
	std::string validationDiag;
	std::string validatedWire;
	const bool publishedPairValid = identityCaptured &&
		readValidatedCacheBufferLocked(appId, validatedWire, validationDiag);
	std::string metadataText;
	cache::CacheMetadataView metadata;
	const bool metadataReady = publishedPairValid &&
		readCacheMetadataFile(appId, metadataText, metadata) &&
		metadata.appId == appId;
	bool memoized = false;
	{
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		memoized = memoizeValidatedPublication(
			g_cacheValidationMemo, /*publicationSucceeded=*/true,
			publishedPairValid,
			identityCaptured
				? std::optional<cache::CacheValidationKey>{publishedKey}
				: std::nullopt,
			CacheValidationResult{true, {}});
		if (memoized && metadataReady)
		{
			const long long now = static_cast<long long>(std::time(nullptr));
			g_cacheProbeMemo[appId] = CacheProbe{
				cache::isBufferReusable(
					true, publishedKey.mtimeSecs, now, provisionTtlSecs())
					? CacheReadiness::Fresh : CacheReadiness::ValidStale,
				metadata.changeNumber};
		}
	}
	if (!memoized && g_pLog)
	{
		g_pLog->debug(
			"AppInfoProvision: published cache pair failed post-write validation "
			"for app=%u: %s\n", appId,
			validationDiag.empty() ? "identity unavailable" : validationDiag.c_str());
	}
	return memoized;
}

bool readValidatedCacheBuffer(uint32_t appId, std::string& buffer)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return false;
	std::string diag;
	return readValidatedCacheBufferLocked(appId, buffer, diag);
}

uint32_t provisionedDlcAppIdForDepot(uint32_t baseAppId, uint32_t depotId)
{
	if (!baseAppId || !depotId) return 0;
	// The planner, BYld and the request-code observer ask about the same
	// depots on every plan: answer repeats for an unchanged pair from memory,
	// without the cache lock or a full buffer read.
	cache::CacheValidationKey identity{};
	if (!statBuffer(baseAppId, identity)) return 0;
	{
		std::lock_guard<std::mutex> lock(g_depotDlcMemoMu);
		const auto found = g_depotDlcMemo.find(baseAppId);
		if (found != g_depotDlcMemo.end() && found->second.identity == identity)
		{
			const auto depot = found->second.dlcByDepot.find(depotId);
			if (depot != found->second.dlcByDepot.end()) return depot->second;
		}
	}
	std::string wire;
	if (!readValidatedCacheBuffer(baseAppId, wire)) return 0;
	const uint32_t dlcAppId = dlcAppIdForDepot(wire, baseAppId, depotId);
	// Stored under the identity seen before the read: a pair replaced in
	// between only costs one more read on the next query.
	std::lock_guard<std::mutex> lock(g_depotDlcMemoMu);
	auto& entry = g_depotDlcMemo[baseAppId];
	if (!(entry.identity == identity)) entry = {identity, {}};
	entry.dlcByDepot[depotId] = dlcAppId;
	return dlcAppId;
}

namespace
{
// Per-app authoritative-set contribution memo. locallyAuthoritativeApps ran once
// per publish and re-read+SHA-validated each app's cache pair, its metadata
// sidecar, and its DLC-metadata YAML (plus a file-lock acquisition per app). On
// a large library that is a second O(N) pass on the hot-reload critical path,
// alongside HotReloadInputs::buildFromCaches. An app's contribution is a pure
// function of its two cache-file mtimes plus its managed/synthetic status, so
// cache it and recompute only when one of those changes.
struct AuthMemoKey
{
	std::int64_t pairMtime = -1;
	std::int64_t dlcMetaMtime = -1;
	bool managed = false;
	bool synthetic = false;

	bool operator==(const AuthMemoKey& o) const
	{
		return pairMtime == o.pairMtime && dlcMetaMtime == o.dlcMetaMtime &&
			managed == o.managed && synthetic == o.synthetic;
	}
};

struct AuthContribution
{
	std::vector<std::uint32_t> ids;
	bool memoizable = true;
};

// Resolve one app's contribution to the authoritative set: the app itself when
// it is authoritative, plus its DLC children when it is an authoritative managed
// base. A transient cache-lock miss is reported non-memoizable so it is retried
// on the next pass rather than frozen as "not authoritative".
AuthContribution computeAuthContribution(
	std::uint32_t appId, bool managed, bool synthetic)
{
	AuthContribution out;
	bool authoritative = false;

	// Preserve the existing marker-only compatibility behavior even after
	// managed-source cleanup moved the readable cache pair aside.
	if (cache::locallyAuthoritative({
			.managed = managed, .active = true, .synthetic = synthetic}))
	{
		authoritative = true;
	}
	else if (managed)
	{
		// Only real contention is a reason to defer: reading the pair under an
		// unusable lock path is still a read, and treating "no trustworthy lock
		// here" as "not authoritative" silently disarmed the outgoing-PICS
		// filter for the app on every publish.
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (cacheLock.heldByAnother())
		{
			out.memoizable = false;
			return out;
		}
		std::string wire;
		std::string validationDiag;
		if (readValidatedCacheBufferLocked(appId, wire, validationDiag))
		{
			std::string metadataText;
			cache::CacheMetadataView metadata;
			if (readCacheMetadataFile(appId, metadataText, metadata) &&
				metadata.appId == appId && cache::locallyAuthoritative({
					.managed = true,
					.active = true,
					.cacheValid = true,
					.hasNormalizedMarker = metadata.hasNormalized,
					.normalized = metadata.normalized}))
			{
				authoritative = true;
			}
		}
	}

	if (!authoritative) return out;
	out.ids.push_back(appId);
	if (managed)
	{
		DlcMetadata::CacheRecord metadata;
		if (readValidatedDlcMetadataCache(appId, 0, metadata))
		{
			std::unordered_set<std::uint32_t> children;
			DlcMetadata::appendChildAppIds(metadata, children);
			out.ids.insert(out.ids.end(), children.begin(), children.end());
		}
	}
	return out;
}

std::mutex g_authMemoMu;
std::unordered_map<std::uint32_t, std::pair<AuthMemoKey, std::vector<std::uint32_t>>>
	g_authMemo;
} // namespace

std::unordered_set<std::uint32_t> locallyAuthoritativeApps(
	const std::unordered_set<std::uint32_t>& managedCandidates,
	const std::unordered_set<std::uint32_t>& activeCandidates)
{
	std::unordered_set<std::uint32_t> authoritative;
	authoritative.reserve(activeCandidates.size());

	std::lock_guard<std::mutex> memoLock(g_authMemoMu);
	for (auto it = g_authMemo.begin(); it != g_authMemo.end();)
		it = activeCandidates.count(it->first) == 0 ? g_authMemo.erase(it)
		                                            : std::next(it);

	for (const std::uint32_t appId : activeCandidates)
	{
		const AuthMemoKey key{
			cachePairMtimeSecs(appId),
			dlcMetadataCacheMtimeSecs(appId),
			managedCandidates.count(appId) != 0,
			isSynthesizedApp(appId),
		};
		const auto it = g_authMemo.find(appId);
		if (it != g_authMemo.end() && it->second.first == key)
		{
			for (const std::uint32_t id : it->second.second)
				authoritative.insert(id);
			continue;
		}
		const AuthContribution contribution =
			computeAuthContribution(appId, key.managed, key.synthetic);
		if (contribution.memoizable)
			g_authMemo[appId] = {key, contribution.ids};
		else
			g_authMemo.erase(appId);
		for (const std::uint32_t id : contribution.ids)
			authoritative.insert(id);
	}
	return authoritative;
}

bool readValidatedDlcMetadataCache(
	std::uint32_t baseAppId,
	std::uint64_t expectedGeneration,
	DlcMetadata::CacheRecord& record)
{
	record = {};
	if (baseAppId == 0) return false;
	std::error_code metadataPathError;
	if (!std::filesystem::is_regular_file(
		getDlcMetadataPath(baseAppId), metadataPathError) || metadataPathError)
		return false;

	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	const bool managed = g_config.managedAppIds.contains(baseAppId);
	const std::uint64_t currentGeneration =
		cachePublicationGenerationLocked(baseAppId);
	if (!managed) return false;

	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return false;
	std::string validatedBaseWire;
	std::string baseValidationDiag;
	if (!readValidatedCacheBufferLocked(
		baseAppId, validatedBaseWire, baseValidationDiag)) return false;
	std::string baseMetadataText;
	cache::CacheMetadataView baseMetadata;
	if (!readCacheMetadataFile(
		baseAppId, baseMetadataText, baseMetadata) ||
		baseMetadata.appId != baseAppId) return false;
	std::string encoded;
	if (!readTextFileBounded(
		getDlcMetadataPath(baseAppId), 32u << 20, encoded)) return false;
	if (!DlcMetadata::decodeCache(encoded, record) ||
		record.baseAppId != baseAppId) return false;
	std::uint8_t baseDigest[20]{};
	Sha1::digest(
		validatedBaseWire.data(), validatedBaseWire.size(), baseDigest);
	const std::string baseSha(
		reinterpret_cast<const char*>(baseDigest), sizeof(baseDigest));
	if (!DlcMetadata::baseIdentityMatches(
		record.baseChangeNumber, record.baseSha,
		baseMetadata.changeNumber, baseSha)) return false;
	if (!DlcMetadata::cacheGenerationMatches(
		record.baseGeneration, expectedGeneration, currentGeneration))
		return false;
	for (const auto& app : record.apps)
	{
		std::string normalized;
		if (!DlcMetadata::normalize(
			app.wireBuffer, app.appid, baseAppId, normalized) ||
			normalized != app.wireBuffer) return false;
		std::uint8_t digest[20]{};
		Sha1::digest(
			app.wireBuffer.data(), app.wireBuffer.size(), digest);
		if (app.sha != std::string(
			reinterpret_cast<const char*>(digest), sizeof(digest))) return false;
	}
	return true;
}

// Caller holds g_cachePublicationMu and the cache file lock. Re-read the
// complete base pair at the publication boundary: a managed generation only
// tracks remove/re-add, while a raw PICS or cross-process writer can replace
// the pair without changing that generation during the CM metadata fetch.
static bool dlcMetadataPublicationMatchesBaseLocked(
	const DlcMetadata::CacheRecord& record,
	std::uint64_t expectedGeneration)
{
	if (record.baseAppId == 0) return false;
	std::string currentWire;
	std::string validationDiag;
	if (!readValidatedCacheBufferLocked(
		record.baseAppId, currentWire, validationDiag)) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	if (!readCacheMetadataFile(
		record.baseAppId, metadataText, metadata) ||
		metadata.appId != record.baseAppId) return false;
	std::uint8_t digest[20]{};
	Sha1::digest(currentWire.data(), currentWire.size(), digest);
	const std::string currentSha(
		reinterpret_cast<const char*>(digest), sizeof(digest));
	return DlcMetadata::publicationMatchesBase(
		record.baseGeneration, expectedGeneration,
		cachePublicationGenerationLocked(record.baseAppId),
		record.baseChangeNumber, record.baseSha,
		metadata.changeNumber, currentSha);
}

bool cacheLockHeldElsewhere(const ProcessLock::FileLock& lock)
{
	if (!lock.usable())
	{
		// In-process writers are serialized by cachePublicationMutex(), pairs
		// are replaced by rename and config.vdf through writeIfUnchanged, so
		// the work stays safe without the cross-process lock.
		static std::atomic<bool> warned{false};
		if (!warned.exchange(true) && g_pLog)
			g_pLog->warn("AppInfoProvision: cache lock %s cannot be used; "
			             "cache work goes ahead without it\n", lock.path().c_str());
	}
	return lock.heldByAnother();
}

std::mutex& cachePublicationMutex()
{
	return g_cachePublicationMu;
}

std::uint64_t cachePublicationGenerationLocked(uint32_t appId)
{
	const auto it = g_cachePublicationGenerations.find(appId);
	return it == g_cachePublicationGenerations.end() ? 0 : it->second;
}

std::string localContentFingerprint(
	uint32_t appId,
	const ManifestStore::ArchivedGidIndex& archivedGids)
{
	std::vector<ProvisionTerminal::LocalInput> inputs;
	for (const uint32_t depotId : DepotKey::managedDepotsForApp(appId))
	{
		const auto key = DepotKey::getCachedKey(depotId);
		inputs.push_back({depotId, key.key, 0});
	}
	const std::string result = fingerprintIndexedLocalInputs(
		std::move(inputs), archivedGids);
	{
		std::lock_guard<std::mutex> lock(g_cachePublicationMu);
		g_localContentFingerprints[appId] = result;
	}
	return result;
}

bool terminalMemoryAppliesLocked(uint32_t appId,
	uint32_t observedChangeNumber, ProvisionOutcome* outcome,
	uint32_t* recordChangeNumber)
{
	const auto it = g_terminalProvisionResults.find(appId);
	if (it == g_terminalProvisionResults.end()) return false;
	if (!cache::terminalResultStillApplies(
		true, it->second.managedGeneration,
		cachePublicationGenerationLocked(appId))) return false;
	const auto fingerprint = g_localContentFingerprints.find(appId);
	const std::string_view currentFingerprint = fingerprint ==
		g_localContentFingerprints.end() ? std::string_view{} : fingerprint->second;
	if (it->second.record.changeNumber == 0)
	{
		if (observedChangeNumber != 0 || it->second.record.appId != appId ||
			(it->second.record.kind != ProvisionTerminal::Kind::VirtualDlc &&
			 it->second.record.inputFingerprint != currentFingerprint))
			return false;
	}
	else if (!ProvisionTerminal::applies(
		it->second.record, appId,
		observedChangeNumber == 0 ? it->second.record.changeNumber
			: observedChangeNumber, currentFingerprint))
		return false;
	if (outcome)
		*outcome = it->second.record.kind == ProvisionTerminal::Kind::VirtualDlc
			? ProvisionOutcome::NotApplicable
			: ProvisionOutcome::NoUsableContent;
	if (recordChangeNumber) *recordChangeNumber = it->second.record.changeNumber;
	return true;
}

bool terminalProvisionResultKnown(uint32_t appId,
	uint32_t observedChangeNumber, ProvisionOutcome* outcome,
	const ManifestStore::ArchivedGidIndex& archivedGids)
{
	const std::string fingerprint = localContentFingerprint(appId, archivedGids);
	// This disk-aware path is reserved for startup and detached workers. The
	// PICS callback uses observeTerminal(), which is memo-only. Keep the lock
	// order publication -> file so a sidecar read cannot race a publication or
	// app removal and republish a record under a newer generation.
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	if (terminalMemoryAppliesLocked(appId, observedChangeNumber,
		outcome, nullptr)) return true;
	if (!g_config.isAddedAppId(appId)) return false;
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return false;
	const auto loaded = ProvisionTerminal::Store(getCacheDir()).load(appId);
	if (loaded.status != ProvisionTerminal::LoadStatus::Valid)
	{
		bool logMalformed = false;
		if (loaded.status == ProvisionTerminal::LoadStatus::Invalid)
			logMalformed = g_loggedMalformedTerminal.insert(appId).second;
		if (logMalformed && g_pLog)
			g_pLog->debug("AppInfoProvision: app=%u terminal sidecar is malformed\n", appId);
		return false;
	}
	if (!ProvisionTerminal::applies(loaded.record, appId,
		observedChangeNumber == 0 ? loaded.record.changeNumber
			: observedChangeNumber, fingerprint)) return false;
	g_terminalProvisionResults[appId] = {
		loaded.record, cachePublicationGenerationLocked(appId)};
	if (outcome)
		*outcome = loaded.record.kind == ProvisionTerminal::Kind::VirtualDlc
			? ProvisionOutcome::NotApplicable
			: ProvisionOutcome::NoUsableContent;
	return true;
}

bool noteTerminalProvisionResult(uint32_t appId, SourceResult result,
	uint32_t changeNumber, const CachePublicationToken& publication,
	ProvisionPassContext& context)
{
	// IncompleteContent (no depot data at all) shares the NoUsableContent
	// persistence shape: both are "no installable content" verdicts bound to
	// the app's change number + local-input fingerprint, so they self-invalidate
	// when Valve publishes depots or the user supplies a depot key/manifest.
	if (result != SourceResult::VirtualDlc &&
	    result != SourceResult::NoUsableContent &&
	    result != SourceResult::IncompleteContent)
		return false;
	ProvisionTerminal::Record record{
		.schema = ProvisionTerminal::kSchema,
		.appId = appId,
		.kind = result == SourceResult::VirtualDlc
			? ProvisionTerminal::Kind::VirtualDlc
			: ProvisionTerminal::Kind::NoUsableContent,
		.changeNumber = changeNumber,
		.inputFingerprint = result == SourceResult::VirtualDlc
			? "-" : localContentFingerprint(appId, context.archived()),
	};
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock) || !cache::cachePublicationAllowed(
		publication.managed, publication.generation,
		cachePublicationGenerationLocked(appId))) return false;
	// Remove any previous verdict before attempting the replacement. A failed
	// sidecar write must remain retryable and must not leave an old in-memory
	// answer authoritative for the current generation.
	g_terminalProvisionResults.erase(appId);
	if (!ProvisionTerminal::Store(getCacheDir()).erase(appId)) return false;
	if (changeNumber != 0 && !ProvisionTerminal::Store(getCacheDir()).publish(record))
		return false;
	g_terminalProvisionResults[appId] = {record, publication.generation};
	return true;
}

TerminalObservation observeTerminal(uint32_t appId,
	uint32_t observedChangeNumber)
{
	TerminalObservation observation;
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	observation.applies = terminalMemoryAppliesLocked(
		appId, observedChangeNumber, &observation.outcome,
		&observation.changeNumber);
	return observation;
}

void primeTerminalMemo(uint32_t appId, std::string_view fingerprint)
{
	if (appId == 0) return;
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	g_localContentFingerprints[appId] = fingerprint;
	if (!g_config.isAddedAppId(appId)) return;
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock)) return;
	const auto loaded = ProvisionTerminal::Store(getCacheDir()).load(appId);
	if (loaded.status != ProvisionTerminal::LoadStatus::Valid ||
		!ProvisionTerminal::applies(
			loaded.record, appId, loaded.record.changeNumber, fingerprint))
		return;
	g_terminalProvisionResults[appId] = {
		loaded.record, cachePublicationGenerationLocked(appId)};
}

CachePublicationToken snapshotCachePublication(uint32_t appId)
{
	// No pass mutex: the PICS callbacks call this on Steam's CM thread. Read
	// the generation first. A config removal publishes the managed set before
	// forgetApp() bumps the generation, so a token that holds the new
	// generation also reads the app as unmanaged.
	CachePublicationToken token;
	{
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		token.generation = cachePublicationGenerationLocked(appId);
	}
	token.managed = g_config.managedAppIds.contains(appId);
	return token;
}

std::mutex& provisioningPassMutex()
{
	return g_provisionPassMu;
}

// Ends an app's provisioning with a source result other than Success: records
// the terminal verdict where one applies and maps the result to the outcome.
ProvisionOutcome outcomeWithoutUpdate(uint32_t appId, SourceResult result,
                                      uint32_t changeNumber,
                                      const CachePublicationToken& publication,
                                      ProvisionPassContext& context)
{
	if (recordsTerminalVerdict(result))
		noteTerminalProvisionResult(appId, result, changeNumber, publication, context);
	switch (result)
	{
	case SourceResult::VirtualDlc:
		return ProvisionOutcome::NotApplicable;
	case SourceResult::NoUsableContent:
		return ProvisionOutcome::NoUsableContent;
	case SourceResult::LocalFailure:
		return ProvisionOutcome::LocalFailure;
	default:
		return ProvisionOutcome::IncompleteContent;
	}
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ProvisionOutcome provisionAppDetailed(uint32_t appId,
                                      ProvisionPassState& pass,
                                      ProvisionPassContext& context,
                                      bool forceRefresh = false)
{
	if (appId == 0) return ProvisionOutcome::IncompleteContent;

	auto publicationIt = context.cachePublications.find(appId);
	if (publicationIt == context.cachePublications.end())
	{
		publicationIt = context.cachePublications.emplace(
		    appId, snapshotCachePublication(appId)).first;
	}
	const CachePublicationToken publication = publicationIt->second;

	// Reuse a validated pair written within the short TTL, so the repeated
	// setup() passes of one boot do not refetch it. Splice eligibility, PICS
	// refreshes and install planning do not depend on this cache age.
	if (!forceRefresh &&
	    cacheUseForApp(appId, false) == cache::CacheUse::Fresh)
	{
		g_pLog->debug("AppInfoProvision: app=%u reusing validated same-boot cache\n",
		              appId);
		return ProvisionOutcome::FreshCache;
	}

	// Native CM batch result (fetched once per provisionAllAddedApps pass,
	// directly from Valve — the PRIMARY source).  Falls through to the
	// steamcmd.net HTTP chain below if this app wasn't in the batch (CM
	// failed, or it was provisioned individually).
	{
		auto it = context.cmBuffers.find(appId);
		if (it != context.cmBuffers.end())
		{
			uint32_t cn = 0;
			if (auto ci = context.cmChanges.find(appId); ci != context.cmChanges.end())
				cn = ci->second;
			const SourceResult cmResult =
			    provisionAppFromCmBuffer(appId, it->second, cn, publication, context);
			if (cmResult == SourceResult::Success)
			{
				g_pLog->debug("AppInfoProvision: app=%u provisioned via CM\n", appId);
				return ProvisionOutcome::Updated;
			}
			if (!shouldTryProviderFallback(cmResult))
			{
				g_pLog->debug(
				    "AppInfoProvision: app=%u CM result is terminal (%s); "
				    "provider fallback suppressed\n", appId,
				    cmResult == SourceResult::NoUsableContent
				        ? "concrete depots are not usable"
				        : cmResult == SourceResult::VirtualDlc
				            ? "DLC has no usable content depots"
				            : cmResult == SourceResult::IncompleteContent
				                ? "no depot data available anonymously"
				                : "local cache write failed");
				return outcomeWithoutUpdate(appId, cmResult, cn, publication, context);
			}
			g_pLog->info("AppInfoProvision: app=%u CM response %s, trying steamcmd\n",
			             appId,
			             cmResult == SourceResult::IncompleteContent
			                 ? "contains no concrete depot data"
			                 : "is invalid");
		}
	}

	if (!pass.shouldAttemptProvider())
	{
		if (cacheUseForApp(appId, true) == cache::CacheUse::Fallback)
		{
			g_pLog->info("AppInfoProvision: app=%u using validated cached buffer "
			             "because live sources are unavailable\n", appId);
			return ProvisionOutcome::FallbackCache;
		}
		return ProvisionOutcome::NetworkUnavailable;
	}

	std::string body, diag;
	std::string url;
	bool fetched = false;
	NetworkFailure finalFailure = NetworkFailure::Provider;
	for (const auto& tmpl : providerChain())
	{
		url = expandUrl(tmpl, appId);
		g_pLog->debug("AppInfoProvision: app=%u GET %s\n", appId, url.c_str());

		// Retry transient failures (timeouts, 5xx, cold-network DNS) with a
		// bounded linear backoff inside the pass's shared provider budget.
		finalFailure = retryNetworkOperation(
			[&] {
				return httpGetJson(
				    url, body, diag,
				    pass.providerOperationTimeoutMs(/*operationCapMs=*/12000));
			},
			/*maxAttempts=*/3, /*baseDelayMs=*/1000,
			[&](int ms) {
				const long delay = pass.providerDelayMs(ms);
				if (delay > 0)
					std::this_thread::sleep_for(std::chrono::milliseconds(delay));
			});
		if (finalFailure == NetworkFailure::None)
		{
			fetched = true;
			pass.noteProviderSuccess();
			break;
		}

		// info, not warn: one provider failing is not actionable while the
		// next provider or the cached pair can still serve the app.
		g_pLog->info("AppInfoProvision: app=%u provider failed after retries (%s), trying next\n",
		             appId, diag.c_str());
		if (pass.providerBudgetExhausted()) break;
	}
	if (!fetched)
	{
		g_pLog->info("AppInfoProvision: app=%u all providers failed\n", appId);
		pass.noteFinalProviderFailure(finalFailure);
		if (pass.providerCircuitOpen() &&
		    cacheUseForApp(appId, true) == cache::CacheUse::Fallback)
		{
			g_pLog->info("AppInfoProvision: app=%u using validated cached buffer "
			             "after provider transport failure\n", appId);
			return ProvisionOutcome::FallbackCache;
		}
		return pass.providerCircuitOpen()
		    ? ProvisionOutcome::NetworkUnavailable
		    : ProvisionOutcome::IncompleteContent;
	}

	YAML::Node appNode;
	std::string err;
	if (!extractAppNode(body, appId, appNode, err))
	{
		g_pLog->info("AppInfoProvision: app=%u parse failed: %s\n", appId, err.c_str());
		return ProvisionOutcome::IncompleteContent;
	}
	const uint32_t changeNumber = pickChangeNumber(appNode);
	const SourceResult result =
	    renderAndPersist(appId, appNode, changeNumber, publication, context);
	if (result == SourceResult::Success)
		return ProvisionOutcome::Updated;
	return outcomeWithoutUpdate(appId, result, changeNumber, publication, context);
}

std::vector<std::uint32_t> publishRuntimeAppInfo(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requested);

void enrichAndPublishDlcMetadata(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& publishedBases);

int provisionAppsPass(const std::unordered_set<uint32_t>& added,
                       bool onlyMissing, ProvisionPassContext& context,
	                       std::unordered_set<uint32_t>* fallbackApps,
	                       const char* origin)
{
	if (added.empty()) return 0;
	// This pass runs in setup(), before Steam shows anything, and writes a
	// cache pair or a verdict per app: one flush when it ends (after the pass
	// lock is released) instead of an fsync of each file and of its folder.
	AtomicFile::DeferSync bulkWrites;
	ProvisionPassSummary summary;
	summary.requested = added.size();

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	coordinator.snapshot([&] {
		const auto managed = g_config.managedAppIds.get();
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		context.cachePublications.clear();
		for (const uint32_t appId : added)
		{
			context.cachePublications.emplace(
			    appId,
			    CachePublicationToken{
			        .managed = managed.count(appId) != 0,
			        .generation = cachePublicationGenerationLocked(appId),
			    });
		}
	});
	// PRIMARY source: one batched anonymous-CM product-info request to
	// Valve for the whole fleet (≈0.3s for dozens of apps; replaces the
	// per-app steamcmd.net round-trips).  Best-effort: any miss falls
	// through to the steamcmd.net HTTP chain inside provisionAppDetailed.  Skip
	// only those apps whose buffer is still fresh on disk (the cache TTL
	// would short-circuit them anyway), so a warm relaunch makes no CM
	// request at all.  Disable entirely via SLSSTEAM_DISABLE_CM=1.
	ProvisionPassState pass;
	const bool cmDisabled = [] {
		const char* v = std::getenv("SLSSTEAM_DISABLE_CM");
		return v && *v && std::string(v) != "0";
	}();
	if (!cmDisabled)
	{
		std::vector<uint32_t> toFetch;
		for (uint32_t appId : added)
		{
			if (!onlyMissing)
				primeTerminalMemo(appId, localContentFingerprint(appId, context.archived()));
			const bool terminalKnown = onlyMissing
				? observeTerminal(appId, 0).applies
				: terminalProvisionResultKnown(appId, 0, nullptr, context.archived());
			if (terminalKnown)
				continue;
			if (onlyMissing)
			{
				// The cold-start caller already found this app's cache pair
				// missing or invalid; do not validate it again here.
				toFetch.push_back(appId);
			}
			else if (cacheUseForApp(appId, false) != cache::CacheUse::Fresh)
			{
				toFetch.push_back(appId);
			}
		}
		if (!toFetch.empty())
		{
			summary.fetched = toFetch.size();
			g_pLog->info("AppInfoProvision: fetching %zu app(s) via native CM\n",
			             toFetch.size());
			const auto cmResult = coordinator.network([&] {
				return CmClient::fetchProductInfoDetailed(
				    toFetch, context.cmBuffers, &context.cmChanges);
			});
			if (cmResult != CmClient::FetchResult::Success)
			{
				context.cmBuffers.clear();
				context.cmChanges.clear();
				pass.noteCmBatchFailure();
				if (cmResult == CmClient::FetchResult::NetworkUnavailable)
				{
					pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
					g_pLog->info("AppInfoProvision: native CM batch found no network; "
					             "using validated local buffers\n");
				}
				else
				{
					g_pLog->info("AppInfoProvision: native CM batch failed, "
					             "probing provider fallback\n");
				}
			}
		}
	}

	int provisioned = 0;
	auto appIt = added.begin();
	while (appIt != added.end())
	{
		const uint32_t appId = *appIt++;
		ProvisionOutcome outcome = ProvisionOutcome::IncompleteContent;
		uint32_t observedChange = 0;
		if (const auto change = context.cmChanges.find(appId);
			change != context.cmChanges.end()) observedChange = change->second;
		const bool terminalKnown = onlyMissing
			? observeTerminal(appId, observedChange).applies
			: terminalProvisionResultKnown(
				appId, observedChange, &outcome, context.archived());
		if (!terminalKnown)
			outcome = provisionAppDetailed(appId, pass, context);
		if (fallbackApps && outcome == ProvisionOutcome::FallbackCache)
			fallbackApps->insert(appId);
		if (outcome == ProvisionOutcome::Updated) ++summary.updated;
		else if (outcome == ProvisionOutcome::FreshCache) ++summary.ready;
		else if (outcome == ProvisionOutcome::FallbackCache) ++summary.fallback;
		else if (isTerminalOutcome(outcome)) ++summary.terminal;
		else ++summary.failed;
		if (isProvisioned(outcome))
		{
			++provisioned;
		}

		// A successful fallback proves that connectivity returned after the
		// initial CM attempt. Give the primary source one recovery batch for
		// every app still pending; success keeps the rest of the fleet off the
		// slower per-app mirror.
		if (pass.takeCmRecoveryRequest() && appIt != added.end())
		{
			std::vector<uint32_t> remaining(appIt, added.end());
			g_pLog->info("AppInfoProvision: provider reachable; retrying native CM "
			             "for %zu remaining app(s)\n", remaining.size());
			const auto recovery = coordinator.network([&] {
				return CmClient::fetchProductInfoDetailed(
				    remaining, context.cmBuffers, &context.cmChanges);
			});
			if (recovery == CmClient::FetchResult::Success)
			{
				g_pLog->info("AppInfoProvision: native CM recovered for remaining apps\n");
			}
			else if (recovery == CmClient::FetchResult::NetworkUnavailable)
			{
				pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
				g_pLog->info("AppInfoProvision: native CM recovery lost connectivity; "
				             "using validated local buffers\n");
			}
			else
			{
				g_pLog->info("AppInfoProvision: native CM recovery failed; "
				             "provider fallback remains active\n");
			}
		}

		const ProvisionNotice notice = noticeForOutcome(outcome);
		if (notice == ProvisionNotice::MetadataUnavailable)
		{
			if (pass.takeConnectivityNotice())
				g_pLog->notifyUser(UserMsg::GameMetadataUnavailable,
				                   std::to_string(appId));
		}
		else if (notice == ProvisionNotice::ReviewGameData)
		{
			// Report the first offender by AppID (the ordinary single-game
			// case) and aggregate the rest into one message after the pass, so
			// a bulk copy cannot produce hundreds of identical popups.
			if (pass.takePreparationNotice(appId))
				g_pLog->notifyUser(UserMsg::GamePreparationFailed,
				                   std::to_string(appId));
		}
		else if (notice == ProvisionNotice::LocalStorage)
		{
			g_pLog->notifyUser(UserMsg::LocalStorageError);
		}
	}
	// One summary for every app that was suppressed above.
	if (const std::size_t suppressed = pass.pendingPreparationSummary())
	{
		g_pLog->notifyUser(UserMsg::GamePreparationFailedBatch,
		                   std::to_string(pass.preparationFailureCount()));
		g_pLog->info(
		    "AppInfoProvision: %zu app(s) without installable content "
		    "(first=%u); %zu notification(s) aggregated\n",
		    pass.preparationFailureCount(), pass.firstPreparationFailure(),
		    suppressed);
	}
	if (provisioned > 0 && summary.terminal == 0)
	{
		g_pLog->info("AppInfoProvision: %d/%zu AdditionalApps provisioned\n",
		             provisioned, added.size());
	}

	// Drop the batch buffers; they can be large and are only needed for
	// this pass.
	context.cmBuffers.clear();
	context.cmChanges.clear();
	g_pLog->info(
		"AppInfoProvision: pass origin=%s requested=%zu fetched=%zu updated=%zu "
		"ready=%zu terminal=%zu failed=%zu\n",
		origin, summary.requested, summary.fetched, summary.updated,
		summary.ready + summary.fallback, summary.terminal, summary.failed);

	return provisioned;
}

int provisionAllAddedApps()
{
	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto added = coordinator.snapshot(
		[] { return g_config.managedAppIds.get(); });
	if (added.empty()) return 0;

	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		added, false, context, nullptr, "startup");
	coordinator.commit([&] {
		if (!injectProtonMappings())
			persistPendingProtonMappings(true);
	});
	return provisioned;
}

ProvisionPassSummary provisionRequestedApps(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requests)
{
	ProvisionPassSummary summary;
	if (requests.empty()) return summary;
	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	ProvisionPassContext context;
	std::vector<RefreshRequest> accepted;
	coordinator.snapshot([&] {
		const auto managed = g_config.managedAppIds.get();
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		for (const RefreshRequest& request : requests)
		{
			if (request.appId == 0 || managed.count(request.appId) == 0)
				continue;
			const std::uint64_t generation =
				cachePublicationGenerationLocked(request.appId);
			if (generation != request.managedGeneration) continue;
			context.cachePublications.emplace(request.appId,
				CachePublicationToken{true, generation});
			accepted.push_back(request);
		}
	});
	if (accepted.empty()) return summary;
	summary.requested = accepted.size();

	std::vector<RefreshRequest> fetch;
	std::vector<RefreshRequest> cachedPublish;
	fetch.reserve(accepted.size());
	cachedPublish.reserve(accepted.size());
	std::size_t busy = 0;
	for (const RefreshRequest& request : accepted)
	{
		const CacheProbe probe = probeCache(
			request.appId, CacheProbeMode::BlockingValidate);
		if (probe.readiness == CacheReadiness::Busy) ++busy;
		if (!requestNeedsFetch(request, probe.readiness, probe.changeNumber))
		{
			++summary.ready;
			if (cachedRuntimePublicationAllowed(request, probe.readiness))
				cachedPublish.push_back(request);
			continue;
		}
		if (terminalProvisionResultKnown(
			request.appId, request.minimumChangeNumber, nullptr, context.archived()))
		{
			++summary.terminal;
			continue;
		}
		fetch.push_back(request);
	}
	const auto planOrigin = [](const std::vector<RefreshRequest>& batch) {
		std::uint8_t reasons = 0;
		for (const auto& request : batch) reasons |= request.reasons;
		if (reasons & reasonMask(RefreshReason::DlcMetadata)) return "dlc-metadata";
		if (reasons & reasonMask(RefreshReason::ForceFull)) return "force-full";
		if (reasons & reasonMask(RefreshReason::LocalInputs)) return "local-inputs";
		if (reasons & reasonMask(RefreshReason::HotAdd)) return "hot-add";
		if (reasons & reasonMask(RefreshReason::CacheRepair)) return "cache-repair";
		if (reasons & reasonMask(RefreshReason::PicsChanges)) return "pics-changes";
		return "pics-product";
	};
	g_pLog->info(
		"AppInfoProvision: plan origin=%s candidates=%zu fetch=%zu ready=%zu "
		"terminal=%zu busy=%zu\n",
		planOrigin(accepted), accepted.size(), fetch.size(), summary.ready,
		summary.terminal, busy);

	ProvisionPassState pass;
	if (!fetch.empty())
	{
		std::vector<uint32_t> appIds;
		appIds.reserve(fetch.size());
		for (const auto& request : fetch) appIds.push_back(request.appId);
		summary.fetched = appIds.size();
		const auto cmResult = coordinator.network([&] {
			return CmClient::fetchProductInfoDetailed(
				appIds, context.cmBuffers, &context.cmChanges);
		});
		if (cmResult != CmClient::FetchResult::Success)
		{
			context.cmBuffers.clear();
			context.cmChanges.clear();
			pass.noteCmBatchFailure();
			if (cmResult == CmClient::FetchResult::NetworkUnavailable)
				pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
		}
	}

	std::vector<RefreshRequest> publish = std::move(cachedPublish);
	for (const RefreshRequest& request : fetch)
	{
		ProvisionOutcome outcome = ProvisionOutcome::IncompleteContent;
		const auto change = context.cmChanges.find(request.appId);
		const uint32_t observed = change == context.cmChanges.end()
			? request.minimumChangeNumber : change->second;
		if (!terminalProvisionResultKnown(
			request.appId, observed, &outcome, context.archived()))
			outcome = provisionAppDetailed(
				request.appId, pass, context, request.forceRefresh);
		if (outcome == ProvisionOutcome::Updated) ++summary.updated;
		else if (outcome == ProvisionOutcome::FreshCache) ++summary.ready;
		else if (outcome == ProvisionOutcome::FallbackCache) ++summary.fallback;
		else if (isTerminalOutcome(outcome)) ++summary.terminal;
		else ++summary.failed;
		if (runtimePublicationAllowed(request.publishRuntime, outcome))
			publish.push_back(request);
	}
	for (const RefreshRequest& request : accepted)
	{
		const std::uint8_t nonMetadataReasons = static_cast<std::uint8_t>(
			request.reasons & ~reasonMask(RefreshReason::DlcMetadata));
		if (nonMetadataReasons == 0)
			publish.push_back(request);
	}

	std::vector<RefreshRequest> runtimePublished;
	coordinator.commit([&] {
		persistPendingProtonMappings(false);
		std::vector<RefreshRequest> basePublish;
		for (const auto& request : publish)
		{
			if ((request.reasons & ~reasonMask(RefreshReason::DlcMetadata)) != 0)
				basePublish.push_back(request);
		}
		if (!basePublish.empty())
		{
			const auto publishedIds =
				publishRuntimeAppInfo(appinfoVdfPath, basePublish);
			const std::unordered_set<std::uint32_t> published(
				publishedIds.begin(), publishedIds.end());
			for (const auto& request : basePublish)
				if (published.count(request.appId) != 0)
					runtimePublished.push_back(request);
		}
	});
	for (const auto& request : runtimePublished)
	{
		if (requiresProton(request.appId))
		{
			(void)OwnerWork::submitCompatReadiness(
				request.appId, request.managedGeneration);
		}
		else
		{
			(void)HotReload::publishPreparedBase(
				request.appId, request.managedGeneration);
		}
	}
	if (dlcEnrichmentRuns(!runtimePublished.empty(), publish))
		enrichAndPublishDlcMetadata(appinfoVdfPath, publish);
	g_pLog->info(
		"AppInfoProvision: pass origin=targeted requested=%zu fetched=%zu "
		"updated=%zu ready=%zu terminal=%zu failed=%zu\n",
		summary.requested, summary.fetched, summary.updated,
		summary.ready + summary.fallback, summary.terminal, summary.failed);
	return summary;
}

int provisionColdStartApps(const std::unordered_set<uint32_t>& candidates)
{
	std::unordered_set<uint32_t> fallbackApps;

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto managedApps = coordinator.snapshot([&] {
		const auto managed = g_config.managedAppIds.get();
		std::unordered_set<uint32_t> scoped;
		for (const uint32_t appId : candidates)
			if (appId != 0 && managed.count(appId) != 0) scoped.insert(appId);
		return scoped;
	});
	// Prime the terminal memo from disk. One manifest observation index for the
	// whole fleet keeps a network-free startup from scaling with apps ×
	// archived manifests, as rebuilding it per app would; the cold pass below
	// records its verdicts against the same index.
	ProvisionPassContext context;
	for (const uint32_t appId : managedApps)
		primeTerminalMemo(appId, localContentFingerprint(appId, context.archived()));
	// Validate persisted pairs and recover only missing or invalid ones; a
	// validated stale pair is already usable and must not trigger synchronous
	// provider work.
	std::vector<std::pair<uint32_t, CacheReadiness>> probed;
	probed.reserve(managedApps.size());
	std::size_t skippedTerminal = 0;
	for (const uint32_t appId : managedApps)
	{
		const CacheProbe cacheProbe =
			probeCache(appId, CacheProbeMode::BlockingValidate);
		// Apps with a terminal content verdict can never satisfy this loop, so
		// including them would refetch the same answer on every pass and keep
		// the pass permanently incomplete.
		const bool terminalKnown =
			observeTerminal(appId, cacheProbe.changeNumber).applies;
		if (terminalKnown)
		{
			++skippedTerminal;
			noteColdRetryOutcome(appId, false);
			continue;
		}
		probed.emplace_back(appId, cacheProbe.readiness);
	}
	std::unordered_set<uint32_t> cold = selectColdStartApps(probed);
	for (auto it = cold.begin(); it != cold.end();)
	{
		if (coldRetryBlocked(*it)) it = cold.erase(it);
		else ++it;
	}
	if (skippedTerminal != 0)
	{
		g_pLog->debug(
		    "AppInfoProvision: skipping %zu app(s) with a known terminal "
		    "content result\n", skippedTerminal);
	}
	if (cold.empty())
	{
		return 0;
	}

	g_pLog->info("AppInfoProvision: cold-start fallback for %zu app(s)\n",
	             cold.size());
	const int provisioned = provisionAppsPass(
		cold, true, context, &fallbackApps, "startup-cold");
	std::size_t unresolvedCount = 0;
	for (const uint32_t appId : cold)
	{
		const bool explicitFallback = fallbackApps.count(appId) != 0;
		if (explicitFallback)
		{
			// Keep the explicit fallback allowlist separate from the PICS
			// suppression set. A fallback may be an explicit raw cache
			// (normalized=false), which must still be replaceable by a newer
			// response. persistAppBuffer() applies the final provenance guard
			// for normalized and legacy pairs.
			noteColdRetryOutcome(appId, false);
			continue;
		}
		const CacheProbe completedProbe = probeCache(
			appId, CacheProbeMode::NonBlockingMemoOnly);
		const bool completedReady =
			completedProbe.readiness == CacheReadiness::Fresh ||
			completedProbe.readiness == CacheReadiness::ValidStale;
		if (!completedReady)
		{
			// A terminal verdict is resolved, not pending: there is nothing a
			// retry could produce. Counting it as unresolved armed the backoff
			// forever and made every later PICS response repeat the pass.
			const auto completedTerminal =
				observeTerminal(appId, completedProbe.changeNumber);
			const bool unresolved = !completedTerminal.applies;
			noteColdRetryOutcome(appId, unresolved);
			if (unresolved) ++unresolvedCount;
			continue;
		}
		noteColdRetryOutcome(appId, false);
	}
	if (unresolvedCount != 0)
	{
		g_pLog->debug(
		    "AppInfoProvision: cold-start fallback incomplete for %zu app(s); "
		    "their subsequent PICS responses are temporarily rate-limited\n",
		    unresolvedCount);
	}

	coordinator.commit([&] {
		if (!injectProtonMappings())
			persistPendingProtonMappings(true);
	});
	return provisioned;
}

bool asyncProvisioningEnabled()
{
	return asyncProvisionEnabled(
		g_config.asyncProvision.get(),
		std::getenv("SLSSTEAM_ASYNC_PROVISION"));
}

void flushPendingProtonMappings()
{
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	// Reconstruct this process-local set from validated normalized caches as
	// well as the pending sidecar. This migrates caches produced by older builds
	// whose platform fallback missed content depots without config.oslist.
	for (const std::uint32_t appId : g_config.managedAppIds.get())
	{
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		std::string wire;
		std::string diag;
		bool proton = false;
		if (!cacheLockHeldElsewhere(cacheLock) &&
			readValidatedCacheBufferLocked(appId, wire, diag, &proton) && proton)
		{
			g_needProton.insert(appId);
		}
	}
	const auto loaded = loadPendingProtonMappings();
	if (!loaded.lockAcquired || loaded.status == PendingProtonFileStatus::Invalid)
	{
		// A lock/read/parse failure is not evidence that the file is empty.
		// Leave both the on-disk mappings and any in-memory retry state intact.
		if (!g_needProton.empty()) (void)injectProtonMappings();
		return;
	}
	if (loaded.status == PendingProtonFileStatus::Missing)
	{
		if (!g_needProton.empty()) (void)injectProtonMappings();
		return;
	}
	g_needProton.insert(loaded.ids.begin(), loaded.ids.end());
	if (g_needProton.empty())
	{
		// This also removes entries left behind by a previous config removal;
		// the valid file was filtered against current management above.
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
		return;
	}
	if (injectProtonMappings())
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
}

std::vector<std::uint32_t> publishRuntimeAppInfo(
    const std::string& appinfoVdfPath,
    const std::vector<RefreshRequest>& requested)
{
	std::unordered_map<std::uint32_t, std::uint64_t> generations;
	{
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		for (const RefreshRequest& request : requested)
			generations[request.appId] =
				cachePublicationGenerationLocked(request.appId);
	}
	const auto selected = selectRuntimePublishCandidates(
		requested, g_config.managedAppIds.get(), SynthMark::loadAll(getCacheDir()),
		generations);
	if (selected.empty())
		return {};

	const std::unordered_set<std::uint32_t> scoped(
		selected.begin(), selected.end());

	// Publish the full managed authority set BEFORE the splice, matching the
	// ordering HotReload::publishAppInfoScopes already uses. The set arms the
	// outgoing-PICS filter that keeps Steam from replacing a spliced record with
	// an empty, token-denied refresh, and it protects every managed app that
	// already has an appinfo entry on disk — none of which depends on this
	// splice succeeding. Publishing after the splice meant one failed live
	// transaction withdrew that protection from unrelated apps, so Steam wiped
	// their `common` section and the library stopped rendering them at all.
	const auto managed = g_config.managedAppIds.get();
	AppInfoState::publishAuthoritative(locallyAuthoritativeApps(
		managed, g_config.addedAppIds.get()));

	if (AppInfoVdf::injectCachedApps(appinfoVdfPath, scoped) == 0)
	{
		g_pLog->warn(
			"AppInfoProvision: live appinfo splice failed for %zu app(s); "
			"restart remains available\n",
			selected.size());
		return {};
	}

	const AppInfoReload::Result reloaded =
		AppInfoState::reloadFromDisk(selected);
	switch (reloaded.status)
	{
		case AppInfoReload::Status::Unavailable:
			g_pLog->warn(
				"AppInfoProvision: live appinfo disk reload unavailable for %zu "
				"app(s); restart remains available\n",
				selected.size());
			return {};
		case AppInfoReload::Status::ReadFailed:
			g_pLog->warn(
				"AppInfoProvision: Steam rejected live appinfo disk reload for %zu "
				"app(s); restart remains available\n",
				selected.size());
			return {};
		case AppInfoReload::Status::Loaded:
			if (!AppInfoReload::allRequestedPresent(reloaded, selected.size()))
			{
				g_pLog->warn(
					"AppInfoProvision: live appinfo reload found %zu/%zu current "
					"app(s); restart remains available\n",
					reloaded.present, selected.size());
				return {};
			}
			else
			{
				g_pLog->info(
					"AppInfoProvision: published %zu app(s) into the live "
					"appinfo cache\n",
					reloaded.present);
			}
			AppInfoVdf::commitLiveEntries(selected);
			return selected;
	}
	return {};
}

struct DlcMetadataWork
{
	DlcMetadata::CacheRecord record;
	std::uint64_t expectedGeneration = 0;
	std::unordered_set<std::uint32_t> candidates;
	bool cacheReady = false;
	bool fetchComplete = false;
	bool publishLive = false;
};

struct DlcMetadataLiveCommitGuard
{
	std::vector<const DlcMetadataWork*> work;
	std::unique_lock<std::mutex> publicationLock;
	std::optional<ProcessLock::FileLock> cacheLock;
};

bool acquireDlcMetadataLiveCommitGuard(void* opaque) noexcept
{
	try
	{
		auto& guard = *static_cast<DlcMetadataLiveCommitGuard*>(opaque);
		guard.publicationLock = std::unique_lock<std::mutex>(g_cachePublicationMu);
		guard.cacheLock.emplace(cacheLockPath(), false);
		// Refuse the commit only when another owner really holds the cache. An
		// unusable lock path is not evidence of a competing writer, and treating
		// it as one abandoned the live DLC splice on every hot add.
		if (guard.cacheLock->heldByAnother()) return false;
		const auto managed = g_config.managedAppIds.get();
		for (const DlcMetadataWork* item : guard.work)
		{
			if (item == nullptr ||
				managed.count(item->record.baseAppId) == 0 ||
				!dlcMetadataPublicationMatchesBaseLocked(
					item->record, item->expectedGeneration)) return false;
		}
		return !guard.work.empty();
	}
	catch (...)
	{
		return false;
	}
}

std::vector<DlcMetadataWork> discoverDlcMetadataWork(
	const std::vector<RefreshRequest>& publishedBases)
{
	std::vector<DlcMetadataWork> work;
	work.reserve(publishedBases.size());
	for (const RefreshRequest& request : publishedBases)
	{
		std::string baseWire;
		if (!readValidatedCacheBuffer(request.appId, baseWire)) continue;
		const auto candidates = selectDlcMetadataCandidates(
			request.appId,
			extractDlcAppIdsBySource(baseWire, request.appId));

		DlcMetadataWork item;
		item.publishLive = dlcMetadataPublishesLive(request);
		item.expectedGeneration = request.managedGeneration;
		item.record.baseAppId = request.appId;
		item.record.baseGeneration = request.managedGeneration;
		std::uint8_t baseDigest[20]{};
		Sha1::digest(baseWire.data(), baseWire.size(), baseDigest);
		item.record.baseSha.assign(
			reinterpret_cast<const char*>(baseDigest), sizeof(baseDigest));
		const CacheProbe probe = probeCache(
			request.appId, CacheProbeMode::BlockingValidate);
		item.record.baseChangeNumber = probe.changeNumber;
		item.candidates.insert(candidates.begin(), candidates.end());
		DlcMetadata::CacheRecord cached;
		if (readValidatedDlcMetadataCache(
			request.appId, request.managedGeneration, cached))
		{
			item.record = std::move(cached);
			item.cacheReady = true;
			item.fetchComplete = true;
		}
		work.push_back(std::move(item));
	}
	return work;
}

void enrichAndPublishDlcMetadata(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& publishedBases)
{
	auto work = discoverDlcMetadataWork(publishedBases);
	if (work.empty()) return;

	std::vector<std::uint32_t> candidates;
	std::unordered_set<std::uint32_t> candidateSeen;
	for (const auto& item : work)
	{
		if (item.cacheReady) continue;
		for (const std::uint32_t appId : item.candidates)
			if (candidateSeen.insert(appId).second) candidates.push_back(appId);
	}
	std::sort(candidates.begin(), candidates.end());
	std::unordered_map<std::uint32_t, std::string> cmBuffers;
	std::unordered_map<std::uint32_t, std::uint32_t> cmChanges;
	if (!candidates.empty())
	{
		g_pLog->info(
			"AppInfoProvision: fetching metadata for %zu DLC candidate(s)\n",
			candidates.size());
		if (CmClient::fetchProductInfoDetailed(
			candidates, cmBuffers, &cmChanges) != CmClient::FetchResult::Success)
		{
			g_pLog->info(
				"AppInfoProvision: DLC metadata fetch unavailable; base publication retained\n");
			return;
		}
	}

	for (auto& item : work)
	{
		if (item.cacheReady) continue;
		item.fetchComplete = std::all_of(
			item.candidates.begin(), item.candidates.end(),
			[&](std::uint32_t candidate) {
				return cmBuffers.count(candidate) != 0;
			});
		if (!item.fetchComplete)
		{
			g_pLog->info(
				"AppInfoProvision: DLC metadata for base=%u was partial; completion remains pending\n",
				item.record.baseAppId);
			continue;
		}
		for (const std::uint32_t candidate : item.candidates)
		{
			const auto buffer = cmBuffers.find(candidate);
			std::string normalized;
			if (buffer == cmBuffers.end() ||
				!DlcMetadata::normalize(
					buffer->second, candidate, item.record.baseAppId, normalized))
			{
				item.record.rejectedAppIds.push_back(candidate);
				continue;
			}
			std::uint8_t digest[20]{};
			Sha1::digest(normalized.data(), normalized.size(), digest);
			const auto change = cmChanges.find(candidate);
			item.record.apps.push_back({
				.appid = candidate,
				.changeNumber = change == cmChanges.end() ? 0 : change->second,
				.sha = std::string(
					reinterpret_cast<const char*>(digest), sizeof(digest)),
				.wireBuffer = std::move(normalized),
			});
		}
		std::sort(item.record.rejectedAppIds.begin(),
		          item.record.rejectedAppIds.end());
	}

	struct CompletedDlcMetadata
	{
		std::uint32_t baseAppId = 0;
		std::uint64_t generation = 0;
		bool publishLive = false;
	};
	std::vector<CompletedDlcMetadata> completedBases;
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		const auto managed = g_config.managedAppIds.get();
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (cacheLockHeldElsewhere(cacheLock)) return;
		for (const auto& item : work)
		{
			if (!item.fetchComplete) continue;
			if (managed.count(item.record.baseAppId) == 0 ||
				!dlcMetadataPublicationMatchesBaseLocked(
					item.record, item.expectedGeneration))
				continue;
			if (!item.cacheReady)
			{
				std::string encoded;
				if (!DlcMetadata::encodeCache(item.record, encoded)) continue;
				std::string error;
				if (!AtomicFile::write(
					getDlcMetadataPath(item.record.baseAppId), encoded, error))
				{
					g_pLog->debug(
						"AppInfoProvision: cannot persist DLC metadata for base=%u: %s\n",
						item.record.baseAppId, error.c_str());
					continue;
				}
			}
			completedBases.push_back({
				item.record.baseAppId, item.expectedGeneration,
				item.publishLive});
		}
	}
	if (completedBases.empty()) return;
	for (const auto& base : completedBases)
	{
		if (!base.publishLive)
			(void)HotReload::noteDlcMetadataCacheCompletion(
				base.baseAppId, base.generation);
	}
	completedBases.erase(
		std::remove_if(
			completedBases.begin(), completedBases.end(),
			[](const auto& base) { return !base.publishLive; }),
		completedBases.end());
	if (completedBases.empty())
	{
		g_pLog->info(
			"AppInfoProvision: cached DLC metadata for cold-start migration; "
			"live Steam state unchanged\n");
		return;
	}

	std::vector<AppInfoVdf::MetadataApp> liveApps;
	std::vector<std::uint32_t> liveIds;
	{
		// Revalidate under the same pass boundary used by config removal. The
		// guarded appinfo transaction acquires appinfo -> publication -> cache,
		// then holds the base identities stable through its CAS. Network and
		// sidecar I/O are already complete before this short commit section.
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		DlcMetadataLiveCommitGuard guard;
		{
			std::unordered_set<std::uint32_t> completedIds;
			for (const auto& base : completedBases)
				completedIds.insert(base.baseAppId);
			for (const auto& item : work)
			{
				if (completedIds.count(item.record.baseAppId) == 0) continue;
				guard.work.push_back(&item);
				for (const auto& app : item.record.apps)
				{
					liveApps.push_back(
						{app.appid, app.changeNumber, app.sha, app.wireBuffer});
					liveIds.push_back(app.appid);
				}
			}
		}
		const int injected = liveApps.empty() ? 0 :
			AppInfoVdf::injectValidatedMetadataAppsGuarded(
				appinfoVdfPath, liveApps, &guard,
				&acquireDlcMetadataLiveCommitGuard);
		guard.cacheLock.reset();
		if (guard.publicationLock.owns_lock()) guard.publicationLock.unlock();
		if (!liveApps.empty() && injected != static_cast<int>(liveApps.size()))
		{
			g_pLog->warn(
				"AppInfoProvision: live DLC metadata splice failed or its base changed; completion remains pending\n");
			return;
		}
		std::sort(liveIds.begin(), liveIds.end());
		liveIds.erase(std::unique(liveIds.begin(), liveIds.end()), liveIds.end());
		if (!liveIds.empty())
		{
			const auto reloaded = AppInfoState::reloadFromDisk(liveIds);
			if (reloaded.status != AppInfoReload::Status::Loaded ||
				reloaded.present != liveIds.size())
			{
				g_pLog->warn(
					"AppInfoProvision: Steam rejected live DLC metadata reload; completion remains pending\n");
				return;
			}
		}
	}
	std::size_t publishedBasesCount = 0;
	for (const auto& base : completedBases)
	{
		if (HotReload::publishMetadataCompletion(
			base.baseAppId, base.generation))
			++publishedBasesCount;
	}
	if (publishedBasesCount == 0) return;

	bool complete = false;
	DlcInjectionIds allDlc;
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		allDlc = collectDlcAppIdsForAddedApps(&complete);
	}
	if (complete) Apps::setDiscoveredAppDlcIds(allDlc.appDlc);
	g_pLog->info(
		"AppInfoProvision: published %zu validated DLC metadata record(s) for %zu base app(s)\n",
		liveApps.size(), publishedBasesCount);
}

void refreshWorkerStartFailed() noexcept
{
	if (g_pLog)
		g_pLog->warn(
		    "AppInfoProvision: unable to start async refresh worker; "
		    "will retry on the next refresh request\n");
}

void finishRefresh(const std::string& completedPath, std::uint64_t token);

bool startRefreshWorker(
    const std::string& appinfoVdfPath, std::uint64_t token,
    const std::vector<RefreshRequest>& requests)
{
	return ThreadStart::startDetached(
		[path = appinfoVdfPath, token, requests]
		{
			ThreadStart::runGuarded(
				[path, requests]
				{
					ScopedNotifySuppression suppression;
					BootProf::Span profile(g_pLog.get(), "provision.async");
					(void)provisionRequestedApps(path, requests);
				},
				[]
				{
					if (g_pLog)
						g_pLog->warn(
						    "AppInfoProvision: async refresh worker failed; will retry\n");
				},
				[path, token] { finishRefresh(path, token); });
		},
		[] { refreshWorkerStartFailed(); });
}

void retainRefreshAfterFailedStart(std::uint64_t token,
                                   const std::vector<RefreshRequest>& requests)
{
	// A known construction failure cannot safely recurse into another thread
	// attempt: persistent resource pressure would otherwise create an
	// unbounded retry loop. Keep the request dormant until the next PICS or
	// config-watcher refresh request reopens the gate.
	std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
	if (g_refreshWorkerToken != token) return;
	g_refreshQueue.restoreAfterStartFailure(requests);
	g_refreshActivePath.clear();
}

void finishRefresh(const std::string& completedPath, std::uint64_t token)
{
	std::string nextPath;
	std::vector<RefreshRequest> nextRequests;
	std::uint64_t nextToken = 0;
	{
		std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
		if (g_refreshWorkerToken != token || !g_refreshQueue.active()) return;
		nextRequests = g_refreshQueue.finishActive();
		if (nextRequests.empty())
		{
			g_refreshActivePath.clear();
			return;
		}
		nextPath = g_refreshActivePath.empty() ? completedPath : g_refreshActivePath;
		nextToken = ++g_refreshWorkerToken;
	}

	if (!startRefreshWorker(nextPath, nextToken, nextRequests))
		retainRefreshAfterFailedStart(nextToken, nextRequests);
}

void refreshInBackground(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requests)
{
	if (requests.empty() || !asyncProvisioningEnabled()) return;
	std::vector<RefreshRequest> batch;
	std::uint64_t token = 0;
	{
		std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
		const RefreshQueueDecision decision = g_refreshQueue.enqueue(requests);
		if (decision.action != RefreshQueueAction::Start) return;
		batch = decision.batch;
		g_refreshActivePath = appinfoVdfPath;
		token = ++g_refreshWorkerToken;
	}

	if (!startRefreshWorker(appinfoVdfPath, token, batch))
		retainRefreshAfterFailedStart(token, batch);
}

DlcInjectionIds collectDlcAppIdsForAddedApps(bool* complete)
{
	if (complete) *complete = false;
	DlcAppIds sources;
	std::unordered_set<uint32_t> taggedSeen;
	std::unordered_set<uint32_t> advertisedSeen;
	std::unordered_set<uint32_t> advertisedWithContent;

	const auto added = g_config.managedAppIds.get();
	if (added.empty())
	{
		if (complete) *complete = true;
		return {};
	}

	// Terminal lookup takes the publication mutex. Resolve it before taking
	// the cache file lock so every path preserves publication -> file order.
	std::unordered_set<uint32_t> terminalApps;
	const auto archivedGids = ManifestStore::archivedGidIndex();
	for (const uint32_t appId : added)
	{
		const std::string fingerprint =
			localContentFingerprint(appId, archivedGids);
		primeTerminalMemo(appId, fingerprint);
		if (observeTerminal(appId, 0).applies)
			terminalApps.insert(appId);
	}

	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (cacheLockHeldElsewhere(cacheLock))
	{
		if (g_pLog)
			g_pLog->debug(
			    "AppInfoProvision: DLC snapshot cache lock busy\n");
		return {};
	}

	// Build this once: each advertised DLC is only a membership lookup after
	// the two manifest directories have been scanned.
	const auto manifestDepotIds = collectValidManifestDepotIds();

	for (uint32_t appId : added)
	{
		if (terminalApps.count(appId) != 0) continue;
		// Validate and retain the wire in one read. Reopening every pair after
		// hashing/parsing it doubled startup I/O for large managed libraries.
		std::string validationDiag;
		std::string wire;
		if (!readValidatedCacheBufferLocked(appId, wire, validationDiag))
		{
			std::error_code bufferEc, metaEc;
			if (cache::dlcSnapshotSkipsApp(
				    std::filesystem::exists(getBufferPath(appId), bufferEc) || bufferEc,
				    std::filesystem::exists(getMetaPath(appId), metaEc) || metaEc))
				continue;
			// A pair that exists but fails validation stops the whole snapshot,
			// and that has to be visible, not a debug line.
			if (g_pLog)
				g_pLog->info(
				    "AppInfoProvision: DLC snapshot rejected cache pair for app=%u "
				    "(%s); DLC injection stays on its previous set this pass\n",
				    appId, validationDiag.c_str());
			return {};
		}

		const auto grouped = extractDlcAppIdsBySource(wire, appId);
		const bool baseHasDlcDepots = hasDepotsInDlc(wire);
		for (uint32_t dlcId : grouped.depotTagged)
		{
			// Never shadow a base AddedApp, and dedup across apps.
			if (!added.count(dlcId))
			{
				appendUnique(sources.depotTagged, taggedSeen, dlcId);
			}
		}
		for (uint32_t dlcId : grouped.advertised)
		{
			if (added.count(dlcId)) continue;
			appendUnique(sources.advertised, advertisedSeen, dlcId);

			// `hasdepotsindlc` is a base-app marker for DLCs with their own
			// appinfo/depots.  A matching depot artifact is the offline
			// fallback for buffers that do not carry the marker.
			if (baseHasDlcDepots || manifestDepotIds.count(dlcId) != 0)
			{
				advertisedWithContent.insert(dlcId);
			}
		}
	}

	const auto selected = selectDlcInjectionIds(
		sources, advertisedWithContent, g_config.injectAllAdvertisedDlc.get());
	if (!selected.appDlc.empty())
	{
		// List the ids, not just counts: the local set may include
		// storefront-only entries that were intentionally kept out of
		// package 0.  Bounded so a large library cannot flood the file.
		constexpr std::size_t kMaxLogged = 24;
		std::string ids;
		for (std::size_t i = 0; i < selected.appDlc.size() && i < kMaxLogged; ++i)
		{
			if (i) ids += ' ';
			ids += std::to_string(selected.appDlc[i]);
		}
		if (selected.appDlc.size() > kMaxLogged)
		{
			ids += " ... (+" +
				std::to_string(selected.appDlc.size() - kMaxLogged) + ")";
		}
		g_pLog->info(
			"AppInfoProvision: collected %zu DLC appid(s) from %zu AdditionalApps: "
			"package0=%zu local=%zu: %s\n",
			selected.appDlc.size(), added.size(), selected.package0.size(),
			selected.appDlc.size(), ids.c_str());
	}
	if (complete) *complete = true;
	return selected;
}

bool forgetAppImpl(uint32_t appId, bool preserveTicketArtifacts)
{
	if (appId == 0) return false;
	// Serialize only the state transition with synchronous and async
	// provisioning passes. Cache and manifest cleanup runs after the lock is
	// released and is protected by its own file/catalog locks.
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		++g_cachePublicationGenerations[appId];
		// Bumping the generation already invalidates any terminal verdict for
		// this id; drop the entry so the map cannot accumulate dead records.
		// Erased inline because the publication mutex is already held here.
		g_terminalProvisionResults.erase(appId);
		{
			std::lock_guard<std::mutex> invalidationLock(
			    g_cacheReadInvalidationMu);
			g_cacheReadInvalidated.insert(appId);
		}
		g_needProton.erase(appId);
		g_pendingProtonRemovals.insert(appId);
	}
	{
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		g_cacheProbeMemo.erase(appId);
		g_cacheValidationMemo.erase(appId);
	}
	{
		std::lock_guard<std::mutex> depotLock(g_depotDlcMemoMu);
		g_depotDlcMemo.erase(appId);
	}

	const auto cacheDir = getCacheDir();
	bool pendingMappingRemoved = false;
	bool complete = false;
	{
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (cacheLockHeldElsewhere(cacheLock)) return false;
		ProvisionTerminal::Store(cacheDir).erase(appId);
		// Decide marker retention only after both publication and cache locks are
		// held. A concurrent synthetic publication must be visible here before
		// the cleanup chooses whether its protection marker is preserved.
		const bool preserveSyntheticMarker =
			cache::shouldPreserveSyntheticMarker(
				preserveTicketArtifacts,
				preserveTicketArtifacts && isSynthesizedAppLocked(appId));
		pendingMappingRemoved = removePendingProtonMappingLocked(appId);

		const std::string suffix =
			".forgotten." +
			std::to_string(static_cast<long long>(std::time(nullptr))) + "." +
			std::to_string(static_cast<long long>(::getpid()));
		const auto records = SynthMark::quarantineAppArtifacts(
			cacheDir, appId, suffix,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		for (const auto& record : records)
		{
			g_pLog->infoOnce("AppInfoProvision: quarantined removed-app cache %s -> %s\n",
			                record.original.string().c_str(),
			                record.quarantined.string().c_str());
		}
		complete = !SynthMark::hasAppArtifacts(
			cacheDir, appId,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		if (!complete)
		{
			g_pLog->warn("AppInfoProvision: app=%u cleanup left one or more cache "
			             "artifacts in place\n", appId);
		}
		g_pLog->infoOnce("AppInfoProvision: forgot app=%u (%zu cache artifact(s), %s)\n",
		             appId, records.size(), complete ? "complete" : "partial");
	}

	if (pendingMappingRemoved)
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		g_pendingProtonRemovals.erase(appId);
	}
	return complete;
}

bool forgetApp(uint32_t appId)
{
	return forgetAppImpl(appId, false);
}

bool forgetManagedSourceApp(uint32_t appId)
{
	return forgetAppImpl(appId, true);
}

void clearCacheReadInvalidation(uint32_t appId)
{
	if (appId == 0) return;
	std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
	g_cacheReadInvalidated.erase(appId);
}

bool cacheMarkerAllowsRead(uint32_t appId)
{
	if (appId == 0) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	return readCacheMetadataFile(appId, metadataText, metadata) &&
		cacheMetadataMarkerAllowsRead(appId, metadata);
}

bool isSynthesizedApp(uint32_t appId)
{
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	return isSynthesizedAppLocked(appId);
}

} // namespace AppInfoProvision
