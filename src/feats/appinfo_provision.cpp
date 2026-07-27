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
#include "manifestid.hpp"
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
#include "../globals.hpp"
#include "../log.hpp"
#include "../bootprof.hpp"
#include "../thread_start.hpp"
#include "../cainfo.hpp"

#include "../utils/ManifestFetch.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/yaml.h"
#include "yaml-cpp/emitter.h"

#include <openssl/sha.h>

#include <curl/curl.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
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

std::mutex g_cacheValidationMu;
std::map<cache::CacheValidationKey, CacheValidationResult> g_cacheValidationMemo;
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
	    g_config.managedAppIds.get().count(appId) == 0)
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



// ---------------------------------------------------------------------------
// libcurl loaded via dlsym to follow the project's portable pattern
// (see utils/ManifestFetch.cpp).  Tied to libcurl.so.4 if available.
// ---------------------------------------------------------------------------

typedef CURL*       (*curl_easy_init_t)();
typedef CURLcode    (*curl_easy_setopt_t)(CURL*, CURLoption, ...);
typedef CURLcode    (*curl_easy_perform_t)(CURL*);
typedef void        (*curl_easy_cleanup_t)(CURL*);
typedef CURLcode    (*curl_easy_getinfo_t)(CURL*, CURLINFO, ...);
typedef const char* (*curl_easy_strerror_t)(CURLcode);

static curl_easy_init_t     p_curl_easy_init     = nullptr;
static curl_easy_setopt_t   p_curl_easy_setopt   = nullptr;
static curl_easy_perform_t  p_curl_easy_perform  = nullptr;
static curl_easy_cleanup_t  p_curl_easy_cleanup  = nullptr;
static curl_easy_getinfo_t  p_curl_easy_getinfo  = nullptr;
static curl_easy_strerror_t p_curl_easy_strerror = nullptr;

bool loadCurl()
{
	if (p_curl_easy_init) return true;
	void* h = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!h) h = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!h) h = RTLD_DEFAULT;
	p_curl_easy_init     = (curl_easy_init_t)     dlsym(h, "curl_easy_init");
	p_curl_easy_setopt   = (curl_easy_setopt_t)   dlsym(h, "curl_easy_setopt");
	p_curl_easy_perform  = (curl_easy_perform_t)  dlsym(h, "curl_easy_perform");
	p_curl_easy_cleanup  = (curl_easy_cleanup_t)  dlsym(h, "curl_easy_cleanup");
	p_curl_easy_getinfo  = (curl_easy_getinfo_t)  dlsym(h, "curl_easy_getinfo");
	p_curl_easy_strerror = (curl_easy_strerror_t) dlsym(h, "curl_easy_strerror");
	return p_curl_easy_init && p_curl_easy_setopt &&
	       p_curl_easy_perform && p_curl_easy_cleanup;
}

std::size_t curlWriteCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
}

NetworkFailure httpGetJson(const std::string& url, std::string& body,
                           std::string& diag, long timeoutMs)
{
	if (!loadCurl()) { diag = "libcurl unavailable"; return NetworkFailure::Provider; }
	if (timeoutMs <= 0)
	{
		diag = "startup network budget exhausted";
		return NetworkFailure::Transient;
	}
	CURL* c = p_curl_easy_init();
	if (!c) { diag = "curl_easy_init failed"; return NetworkFailure::Provider; }

	body.clear();
	p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	// Bounded timeouts.  AppInfoProvision runs in setup() on the startup
	// path, so we must not stall Steam's launch for too long if
	// steamcmd.net is slow or unreachable.  The fetch is now wrapped in a
	// bounded retry (see provisionApp), and all provider attempts in this
	// startup pass share one 15-second wall-clock budget. A slow mirror can
	// therefore never multiply this timeout by app count or retry count.
	p_curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
	p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,
	                   std::min(timeoutMs, 8000L));
	// Same multi-thread safety justification as ManifestFetch::httpGet.
	p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-AppInfoProvision/0.1");
	// Pin the system trust store (see cainfo.hpp): the libcurl Steam loads
	// otherwise fails CA verification on SteamOS/Arch with curl error 60
	// ("Peer certificate cannot be authenticated"), the exact failure that
	// sinks the steamcmd.net fallback. No-op when no bundle is found.
	if (const char* f = ca::bundleFile()) p_curl_easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  p_curl_easy_setopt(c, CURLOPT_CAPATH, d);

	const CURLcode rc = p_curl_easy_perform(c);
	long status = 0;
	if (p_curl_easy_getinfo) p_curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	p_curl_easy_cleanup(c);

	if (rc != CURLE_OK)
	{
		diag = p_curl_easy_strerror ? p_curl_easy_strerror(rc) : "curl error";
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

// ---------------------------------------------------------------------------
// VDF text writer.
// ---------------------------------------------------------------------------
//
// `AppInfoVdf::translateWireToIndexed` accepts the same KV1-text dialect
// that PICS uses on the wire ("appinfo" { ... } with quoted keys/values
// and braces).  We emit a strict subset of that here: every leaf is a
// quoted string (Steam's appinfo schema is all-strings anyway).
//
// We also keep a fallback v39-binary writer in case the project ever
// switches to that.  Currently unused.

void emitEscaped(std::string& out, const std::string& s)
{
	out.push_back('"');
	for (char c : s)
	{
		if (c == '\\' || c == '"') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
}

void emitNode(std::string& out, const YAML::Node& node, int depth);

void emitMap(std::string& out, const YAML::Node& node, int depth)
{
	for (auto it = node.begin(); it != node.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();
		// SteamCMD JSON adds "_change_number", "_sha", "_size",
		// "_missing_token" sibling fields at the top level.  These
		// belong to the *envelope*, not the appinfo body, so the
		// caller filters them before calling emitMap.
		out.append(depth, '\t');
		emitEscaped(out, key);
		const auto& val = it->second;
		if (val.IsMap())
		{
			out.append("\n");
			out.append(depth, '\t');
			out.append("{\n");
			emitNode(out, val, depth + 1);
			out.append(depth, '\t');
			out.append("}\n");
		}
		else if (val.IsScalar())
		{
			out.append("\t\t");
			emitEscaped(out, val.as<std::string>());
			out.append("\n");
		}
		else if (val.IsSequence())
		{
			// Sequences shouldn't appear in appinfo, but if they do,
			// render as an indexed map: "0" "v0" "1" "v1"...
			out.append("\n");
			out.append(depth, '\t');
			out.append("{\n");
			int idx = 0;
			for (const auto& child : val)
			{
				out.append(depth + 1, '\t');
				emitEscaped(out, std::to_string(idx++));
				if (child.IsMap())
				{
					out.append("\n");
					out.append(depth + 1, '\t');
					out.append("{\n");
					emitNode(out, child, depth + 2);
					out.append(depth + 1, '\t');
					out.append("}\n");
				}
				else
				{
					out.append("\t\t");
					emitEscaped(out, child.IsScalar() ? child.as<std::string>() : std::string{});
					out.append("\n");
				}
			}
			out.append(depth, '\t');
			out.append("}\n");
		}
		else
		{
			// Null / undefined → empty string
			out.append("\t\t\"\"\n");
		}
	}
}

void emitNode(std::string& out, const YAML::Node& node, int depth)
{
	if (node.IsMap())
	{
		emitMap(out, node, depth);
	}
}

// Strip depots that we have no decryption key for so Steam's
// downloader picks a depot we *can* actually decrypt.
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
//   - Removes a DLC from extended.listofdlc when all of that DLC's content
//     depots were dropped.  Otherwise PackagePatch advertises the DLC as
//     owned and Steam independently plans the rejected encrypted content.
//   - If the surviving set has at least one playable depot but the
//     original "common.oslist" included an OS we just dropped, narrow
//     "oslist" to the OSes we still have so Steam picks Proton (for
//     "windows") rather than a phantom native binary.
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
	int kept = 0;
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
		if (depotPublicManifestIsEmpty(depotNode))
		{
			++dropped;
			g_pLog->info("AppInfoProvision: app=%u dropping empty depot %u "
			             "(public manifest size 0)\n", appId, depotId);
			continue;
		}

		// DLC entries have no `manifests` block; they're virtual and
		// don't need a decryption key — keep them.
		const auto savedKey = DepotKey::getCachedKey(depotId);
		const bool hasKey = !savedKey.key.empty();

		if (!hasKey && !isVirtualDlc)
		{
			++dropped;
			continue;  // omit from newDepots
		}
		++kept;
		newDepots[key] = YAML::Clone(depotNode);
		if (dlcAppId != 0)
		{
			usableDlcAppIds.insert(dlcAppId);
		}

		// Track which OS this surviving depot supports so we can
		// narrow common.oslist later.
		if (depotNode.IsMap() && depotNode["config"] &&
		    depotNode["config"]["oslist"])
		{
			std::string osStr;
			try { osStr = depotNode["config"]["oslist"].as<std::string>(); }
			catch (...) {}
			std::size_t i = 0;
			while (i < osStr.size())
			{
				std::size_t j = osStr.find(',', i);
				if (j == std::string::npos) j = osStr.size();
				const auto piece = osStr.substr(i, j - i);
				if (!piece.empty()) survivingOs.insert(piece);
				i = j + 1;
			}
		}
	}

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
		if (!survivingOs.empty() && !survivingOs.count("linux"))
		{
			markProtonNeeded(appId, publication);
		}
		return;
	}

	body["depots"] = newDepots;
	g_pLog->info("AppInfoProvision: app=%u dropped %d/%d unsupported depots (kept %d)\n",
	             appId, dropped, totalNumeric, kept);

	// If none of the surviving depots target Linux natively, the app
	// can only run through Proton.  Register it for a CompatToolMapping
	// so Steam downloads + runs the windows depot on Linux instead of
	// skipping it as "no applicable platform".  (Covers windows-only
	// and windows+macos apps.)
	if (!survivingOs.empty() && !survivingOs.count("linux"))
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

// Forward declaration: defined with the on-disk cache helpers below.
const std::string& getCacheDir();
bool readValidatedCacheBufferLocked(uint32_t appId, std::string& wireOut,
                                    std::string& diag);

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
int synthesizeDepotsFromStore(YAML::Node& body, uint32_t appId)
{
	std::vector<ManifestSynth::SynthDepot> depots;
	// Accumulate file lists per OS across all of the app's depots so we can
	// synthesize one launch entry per platform (native + forced-Proton).
	std::vector<std::string> winFiles, linFiles, macFiles;
	for (uint32_t depotId : DepotKey::managedDepotsForApp(appId))
	{
		const uint64_t gid = ManifestStore::bestArchivedGid(depotId, /*excludeGid=*/0);
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

// Neutralize the legacy third-party CD-key requirement.
//
// appinfo's `extended/hadthirdpartycdkey "1"` makes Steam's launch
// pipeline run a GettingLegacyKey step: it issues ClientGetLegacyGameKey
// to the CM, which validates ownership server-side and answers
// AccessDenied (EResult 15) for an app the account doesn't actually own.
// The launch then fails BEFORE the compat tool / game process is ever
// spawned — the "updating product key" flash that drops straight back to
// Play (console_log: "LaunchApp failed with GettingLegacyKey with 15",
// and no ~/steam-<appid>.log because Proton never starts).
//
// The IClientUser::RequiresLegacyCDKey detour only suppresses the CD-key
// *prompt* (ShowCDKey) path; it does NOT gate this launch-time fetch,
// which reads straight off appinfo.  Zeroing the field here — in the same
// offline appinfo rewrite that prunes depots and pins gids, NOT on the
// live product-info buffer (which Steam sha-validates) — makes the launch
// skip GettingLegacyKey entirely.  The game's own activation DRM (EA
// serial, Uplay, ...) is a separate layer untouched by this.
void neutralizeLegacyCdKey(YAML::Node& body, uint32_t appId)
{
	if (!body.IsMap()) return;
	YAML::Node ext = body["extended"];
	if (!ext || !ext.IsMap()) return;
	YAML::Node had = ext["hadthirdpartycdkey"];
	if (!had || !had.IsScalar()) return;

	std::string cur;
	try { cur = had.as<std::string>(); } catch (...) { return; }
	if (cur == "0") return;

	ext["hadthirdpartycdkey"] = "0";
	g_pLog->info("AppInfoProvision: app=%u cleared extended.hadthirdpartycdkey "
	             "(was %s) so launch skips GettingLegacyKey\n",
	             appId, cur.c_str());
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
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return value == "dlc";
}

SourceResult renderAppinfoBuffer(
    const YAML::Node& appNode, uint32_t appId, std::string& wireOut,
    const CachePublicationToken& publication, bool* synthesizedOut)
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
	// pruning. A valid token-limited response with no depot data may benefit
	// from a secondary provider; concrete depots that are all rejected locally
	// will not, so that result must be terminal.
	bool hadConcreteContent = hasUsableContentDepot(body);

	// Token-locked apps (product-info access token denied to anonymous
	// sessions) arrive with NO depots.  Rebuild the block from the depot
	// key + archived manifest we already hold, so the prune/Proton/splice
	// tail below runs unchanged.  No-op when real depots are present.
	if (YAML::Node d = body["depots"]; !d || !d.IsMap() || d.size() == 0)
	{
		const int n = synthesizeDepotsFromStore(body, appId);
		if (n > 0)
		{
			if (synthesizedOut) *synthesizedOut = true;
			g_pLog->info("AppInfoProvision: app=%u synthesized %d depot(s) from "
			             "stored manifests (product-info had none)\n", appId, n);
			hadConcreteContent = hasUsableContentDepot(body);
		}
	}

	// Strip depots we can't decrypt; narrow common.oslist accordingly.
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

	// Clear the launch-time legacy CD-key gate (see helper above): without
	// this the GettingLegacyKey step fails AccessDenied for an unowned app
	// and the launch aborts before the game/Proton ever starts.
	neutralizeLegacyCdKey(body, appId);

	// A locked app keeps the LIVE public gid in its provisioned appinfo so
	// Steam computes a genuine content delta; the pinned target is applied at
	// the post-commit reconcile instead, which avoids contaminating the
	// active/baseline manifest read.

	// Steam's appinfo wire format wraps the document in "appinfo" { ... }.
	wireOut.clear();
	wireOut.append("\"appinfo\"\n{\n");
	emitNode(wireOut, body, 1);
	wireOut.append("}\n");
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
		if (!std::filesystem::exists(value))
		{
			std::error_code ec;
			std::filesystem::create_directories(value, ec);
		}
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
	if (!cacheLock.acquired()) return;

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
// whether it exists and is non-empty.  Used by provisionApp's short-lived
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
	if (!cacheLock.acquired())
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
	std::string writeError;
	if (!publishCachePairLocked(appId, wire, metadata, markSynthetic,
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
// Decide whether a given appId already has depots in the on-disk
// appinfo.vdf.  We don't fully parse v41 here — we only care about
// finding the literal "depots" key inside that app's binary KV blob,
// which is enough to skip already-rich entries.
// ---------------------------------------------------------------------------

[[maybe_unused]]
bool hasDepotsForApp(const std::string& appinfoVdfPath, uint32_t appId)
{
	std::ifstream ifs(appinfoVdfPath, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return false;
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (1LL << 30)) return false;
	std::string buf;
	buf.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	ifs.read(buf.data(), sz);

	// Linear scan for the appid little-endian followed by enough header
	// bytes to be a real entry header.  The appinfo.vdf entry header
	// layout is:
	//   uint32 appid; uint32 size; uint32 info_state; uint64 last_updated;
	//   uint64 token; bytes sha[20]; uint32 change#; bytes binsha[20]
	// total = 72 bytes.
	const auto* data = reinterpret_cast<const std::uint8_t*>(buf.data());
	const std::size_t n = buf.size();
	if (n < 76) return false;

	for (std::size_t i = 0; i + 76 <= n; ++i)
	{
		uint32_t a;
		std::memcpy(&a, data + i, sizeof(a));
		if (a != appId) continue;
		uint32_t entrySize;
		std::memcpy(&entrySize, data + i + 4, sizeof(entrySize));
		// sanity
		if (entrySize < 64 || entrySize > 8u * 1024u * 1024u) continue;
		const std::size_t kvStart = i + 72;
		const std::size_t kvEnd   = i + 8 + entrySize;
		if (kvEnd > n || kvEnd <= kvStart) continue;
		const std::string_view kv(reinterpret_cast<const char*>(data) + kvStart,
		                          kvEnd - kvStart);
		// Binary v41 KV uses uint32 string-table indices, so "depots"
		// won't appear as ASCII inside the body.  We instead look for
		// "manifests" or any depot-like ASCII fragment that the wire
		// reader writes raw.  Safer: the binary VDF *also* embeds
		// string values verbatim, including manifest gids and section
		// labels like "public", but those are noisy.  Use a simple
		// heuristic: the entry must contain the literal "manifests"
		// somewhere — every depot row carries it as a value.
		if (kv.find("manifests") != std::string_view::npos)
		{
			return true;
		}
		// Even if the v41 binary blob is fully indexed, the original
		// raw string table at the file end resolves them — but for our
		// purposes "depot key not yet pinned" means we provision.
		return false;
	}
	return false;
}

// ---------------------------------------------------------------------------
// SHA-1 helper (libcrypto via dlsym, same pattern as appinfo_vdf.cpp).
// ---------------------------------------------------------------------------

void sha1BytesInternal(const void* data, std::size_t n, std::uint8_t out[20])
{
	static unsigned char* (*p_SHA1)(const unsigned char*, size_t, unsigned char*) = nullptr;
	if (!p_SHA1)
	{
		void* h = dlopen("libcrypto.so.3", RTLD_NOLOAD | RTLD_LAZY);
		if (!h) h = dlopen("libcrypto.so.3", RTLD_LAZY);
		if (!h) h = dlopen("libcrypto.so.1.1", RTLD_LAZY);
		if (!h) h = RTLD_DEFAULT;
		p_SHA1 = (unsigned char* (*)(const unsigned char*, size_t, unsigned char*))
		         dlsym(h, "SHA1");
	}
	if (p_SHA1)
	{
		p_SHA1(reinterpret_cast<const unsigned char*>(data), n, out);
	}
	else
	{
		std::memset(out, 0, 20);
	}
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
	if (!std::filesystem::exists(path)) return false;

	// This function is restricted to setup()'s preinit window. Runtime PICS
	// workers persist a pending set instead; Steam's ConfigStore writers do
	// not participate in our advisory lock, so a stat-then-rename check here
	// would still be a TOCTOU race against Steam.
	(void)getCacheDir();
	const auto configLockPath = cacheLockPath();
	ProcessLock::FileLock configLock(configLockPath, false);
	if (!configLock.acquired())
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
		const std::string key = "\"" + std::to_string(appId) + "\"";
		// Already mapped (by us or the user)?  Search only within the
		// mapping block to avoid matching the same id elsewhere.
		// Cheap: search from mapBrace forward; CompatToolMapping is
		// near the end of the file in practice.
		if (content.find(key, mapBrace) != std::string::npos)
		{
			continue;
		}
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

// ---------------------------------------------------------------------------
// Native CM provider: parse the wire-text VDF buffer the anonymous CM
// returns into the same YAML::Node shape extractAppNode produces from
// steamcmd's JSON, so it flows through the identical prune/render/persist
// path.  The CM buffer is `"appinfo" { ... }` KV1 text;
// we parse its inner body into a map.
// ---------------------------------------------------------------------------

// Minimal KV1-text reader: builds a YAML::Node tree from a VDF-text body.
// `p`/`end` bracket the buffer.  Returns the node for the object whose
// opening brace has already been consumed by the caller (or, at top
// level, the single "appinfo" wrapper's body).  Defensive: bails to an
// empty node on malformed input.
class CmVdfReader
{
public:
	CmVdfReader(const char* p, const char* end) : p_(p), end_(end) {}

	// Parse the top-level `"appinfo" { ... }` and return the inner body
	// node (equivalent to steamcmd's data[appid]).  Empty on failure.
	YAML::Node parseAppinfo()
	{
		std::string key;
		Tok t = next(key);
		if (t != Tok::String) return YAML::Node(YAML::NodeType::Undefined);
		t = next(key /*reused as scratch*/);
		// After the top key we expect an opening brace.
		if (t != Tok::OpenBrace) return YAML::Node(YAML::NodeType::Undefined);
		return parseObject();
	}

private:
	enum class Tok { String, OpenBrace, CloseBrace, End };

	YAML::Node parseObject()
	{
		YAML::Node node(YAML::NodeType::Map);
		std::string key;
		for (;;)
		{
			Tok t = next(key);
			if (t == Tok::CloseBrace || t == Tok::End) break;
			if (t != Tok::String) break; // malformed
			std::string val;
			Tok vt = next(val);
			if (vt == Tok::OpenBrace)
			{
				node[key] = parseObject();
			}
			else if (vt == Tok::String)
			{
				node[key] = val;
			}
			else
			{
				break; // malformed
			}
		}
		return node;
	}

	Tok next(std::string& out)
	{
		out.clear();
		skipWs();
		if (p_ >= end_) return Tok::End;
		const char c = *p_;
		if (c == '{') { ++p_; return Tok::OpenBrace; }
		if (c == '}') { ++p_; return Tok::CloseBrace; }
		if (c == '"') return readQuoted(out);
		return readBare(out);
	}

	void skipWs()
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p_; continue; }
			if (c == '/' && p_ + 1 < end_ && p_[1] == '/')
			{
				while (p_ < end_ && *p_ != '\n') ++p_;
				continue;
			}
			break;
		}
	}

	Tok readQuoted(std::string& out)
	{
		++p_;
		while (p_ < end_)
		{
			const char c = *p_++;
			if (c == '"') return Tok::String;
			if (c == '\\' && p_ < end_)
			{
				const char e = *p_++;
				switch (e)
				{
					case 'n':  out.push_back('\n'); break;
					case 't':  out.push_back('\t'); break;
					case 'r':  out.push_back('\r'); break;
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					default:   out.push_back(e);    break;
				}
				continue;
			}
			out.push_back(c);
		}
		return Tok::End;
	}

	Tok readBare(std::string& out)
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
			    c == '{' || c == '}' || c == '"') break;
			out.push_back(*p_++);
		}
		return Tok::String;
	}

	const char* p_;
	const char* end_;
};

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
                                    std::string& diag)
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

		std::uint8_t digestBytes[20]{};
		sha1BytesInternal(wire.data(), wire.size(), digestBytes);
		const std::string actualSha(
		    reinterpret_cast<const char*>(digestBytes), sizeof(digestBytes));

		CmVdfReader reader(wire.data(), wire.data() + wire.size());
		const YAML::Node appNode = reader.parseAppinfo();
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
	if (!cacheLock.acquired()) return cache::CacheUse::None;

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
	{
		std::lock_guard<std::mutex> lk(g_cacheValidationMu);
		auto it = g_cacheValidationMemo.find(key);
		if (it != g_cacheValidationMemo.end())
		{
			result = it->second;
		}
		else
		{
			result.valid = hasValidatedCachedBuffer(appId, result.diag);
			g_cacheValidationMemo.emplace(key, result);
		}
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
	if (!cacheLock.acquired())
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
		const auto found = g_cacheValidationMemo.find(key);
		if (found != g_cacheValidationMemo.end())
		{
			validation = found->second;
			memoHit = true;
		}
	}
	if (!memoHit)
	{
		validation.valid = hasValidatedCachedBuffer(appId, validation.diag);
		std::lock_guard<std::mutex> validationLock(g_cacheValidationMu);
		g_cacheValidationMemo.emplace(key, validation);
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
                              const CachePublicationToken& publication)
{
	std::string wire;
	bool synthesized = false;
	const SourceResult renderResult = renderAppinfoBuffer(
	    appNode, appId, wire, publication, &synthesized);
	if (renderResult != SourceResult::Success)
	{
		const char* reason = "invalid response";
		if (renderResult == SourceResult::IncompleteContent)
			reason = "response contains no concrete depot data";
		else if (renderResult == SourceResult::NoUsableContent)
			reason = "concrete depots are not usable";
		else if (renderResult == SourceResult::VirtualDlc)
			reason = "DLC has no usable content depots";
		g_pLog->info("AppInfoProvision: app=%u render stopped (%s)\n", appId, reason);
		return renderResult;
	}
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->info("AppInfoProvision: app=%u buffer has no depots, skipping\n", appId);
		return SourceResult::IncompleteContent;
	}

	std::string sha20;
	{
		std::uint8_t tmp[20];
		sha1BytesInternal(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	if (!persistBuffer(appId, changeNumber, sha20, wire, publication,
	                   synthesized))
	{
		g_pLog->info("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return SourceResult::LocalFailure;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return SourceResult::Success;
}

// Provision one app from a native-CM wire buffer. Mirrors the steamcmd
// path's tail but skips the JSON parse — the CM buffer is already wire VDF.
SourceResult provisionAppFromCmBuffer(uint32_t appId, const std::string& cmWire,
                                      uint32_t changeNumber,
                                      const CachePublicationToken& publication)
{
	if (cmWire.empty()) return SourceResult::InvalidResponse;
	CmVdfReader reader(cmWire.data(), cmWire.data() + cmWire.size());
	YAML::Node appNode = reader.parseAppinfo();
	if (!appNode || !appNode.IsMap() || appNode.size() == 0)
	{
		g_pLog->info("AppInfoProvision: app=%u CM buffer parse failed, fallback\n", appId);
		return SourceResult::InvalidResponse;
	}
	return renderAndPersist(appId, appNode, changeNumber, publication);
}

struct ProvisionPassContext
{
	std::unordered_map<uint32_t, std::string> cmBuffers;
	std::unordered_map<uint32_t, uint32_t> cmChanges;
	std::unordered_map<uint32_t, CachePublicationToken> cachePublications;
};

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
	if (!cacheLock.acquired()) return {};

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
	if (!cacheLock.acquired()) return false;

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

CacheProbe probeCache(uint32_t appId, CacheProbeMode mode)
{
	return probeCacheImpl(appId, mode);
}

std::int64_t cachePairMtimeSecs(uint32_t appId) noexcept
{
	return cachePairMtimeSecsImpl(appId);
}

void sha1Bytes(const void* data, std::size_t size, std::uint8_t out[20])
{
	sha1BytesInternal(data, size, out);
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
	if (!cacheLock.acquired()) return false;
	std::string diag;
	return readValidatedCacheBufferLocked(appId, buffer, diag);
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
	const bool managed = g_config.managedAppIds.get().count(baseAppId) != 0;
	const std::uint64_t currentGeneration =
		cachePublicationGenerationLocked(baseAppId);
	if (!managed) return false;

	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return false;
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
	sha1BytesInternal(
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
		sha1BytesInternal(
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
	sha1BytesInternal(currentWire.data(), currentWire.size(), digest);
	const std::string currentSha(
		reinterpret_cast<const char*>(digest), sizeof(digest));
	return DlcMetadata::publicationMatchesBase(
		record.baseGeneration, expectedGeneration,
		cachePublicationGenerationLocked(record.baseAppId),
		record.baseChangeNumber, record.baseSha,
		metadata.changeNumber, currentSha);
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

std::string localContentFingerprint(uint32_t appId)
{
	return localContentFingerprint(appId, ManifestStore::archivedGidIndex());
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
	uint32_t observedChangeNumber, ProvisionOutcome* outcome)
{
	const std::string fingerprint = localContentFingerprint(appId);
	// This disk-aware path is reserved for startup and detached workers. The
	// PICS callback uses observeTerminal(), which is memo-only. Keep the lock
	// order publication -> file so a sidecar read cannot race a publication or
	// app removal and republish a record under a newer generation.
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	if (terminalMemoryAppliesLocked(appId, observedChangeNumber,
		outcome, nullptr)) return true;
	if (!g_config.isAddedAppId(appId)) return false;
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return false;
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
	uint32_t changeNumber, const CachePublicationToken& publication)
{
	if (result != SourceResult::VirtualDlc && result != SourceResult::NoUsableContent)
		return false;
	ProvisionTerminal::Record record{
		.schema = ProvisionTerminal::kSchema,
		.appId = appId,
		.kind = result == SourceResult::VirtualDlc
			? ProvisionTerminal::Kind::VirtualDlc
			: ProvisionTerminal::Kind::NoUsableContent,
		.changeNumber = changeNumber,
		.inputFingerprint = result == SourceResult::VirtualDlc
			? "-" : localContentFingerprint(appId),
	};
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired() || !cache::cachePublicationAllowed(
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

void clearTerminalProvisionResult(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	g_terminalProvisionResults.erase(appId);
	ProvisionTerminal::Store(getCacheDir()).erase(appId);
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
	if (!cacheLock.acquired()) return;
	const auto loaded = ProvisionTerminal::Store(getCacheDir()).load(appId);
	if (loaded.status != ProvisionTerminal::LoadStatus::Valid ||
		!ProvisionTerminal::applies(
			loaded.record, appId, loaded.record.changeNumber, fingerprint))
		return;
	g_terminalProvisionResults[appId] = {
		loaded.record, cachePublicationGenerationLocked(appId)};
}

void primeTerminalMemo(uint32_t appId)
{
	primeTerminalMemo(appId, localContentFingerprint(appId));
}

CachePublicationToken snapshotCachePublication(uint32_t appId)
{
	CachePublicationToken token;
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	token.managed = g_config.managedAppIds.get().count(appId) != 0;
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	token.generation = cachePublicationGenerationLocked(appId);
	return token;
}

std::mutex& provisioningPassMutex()
{
	return g_provisionPassMu;
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ProvisionOutcome provisionAppDetailed(uint32_t appId,
                                      const std::string& appinfoVdfPath,
                                      ProvisionPassState& pass,
                                      ProvisionPassContext& context,
                                      bool forceRefresh = false)
{
	(void)appinfoVdfPath;
	if (appId == 0) return ProvisionOutcome::IncompleteContent;

	auto publicationIt = context.cachePublications.find(appId);
	if (publicationIt == context.cachePublications.end())
	{
		publicationIt = context.cachePublications.emplace(
		    appId, snapshotCachePublication(appId)).first;
	}
	const CachePublicationToken publication = publicationIt->second;

	// Short-lived on-disk cache to tame startup cost.  Steam re-execs
	// setup() several times during a single cold boot (observed 4x on
	// the Zorin VM), and each pass would otherwise issue one synchronous
	// HTTP GET per AddedApp — so the boot cost grew O(n_apps * n_passes)
	// and stalled Steam's launch the more games the user added.
	//
	// If we already wrote picsbuffer_<appid>.bin within the short TTL,
	// reuse it for this pass. Startup splice eligibility is decided separately
	// by complete-pair validation; observed PICS change numbers schedule the
	// precise background refresh. Install planning still chooses and freezes
	// the exact public/local/archive manifest independently of this cache age.
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
			    provisionAppFromCmBuffer(appId, it->second, cn, publication);
			if (cmResult == SourceResult::Success)
			{
				g_pLog->info("AppInfoProvision: app=%u provisioned via CM\n", appId);
				return ProvisionOutcome::Updated;
			}
			if (!shouldTryProviderFallback(cmResult))
			{
				g_pLog->info(
				    "AppInfoProvision: app=%u CM result is terminal (%s); "
				    "provider fallback suppressed\n", appId,
				    cmResult == SourceResult::NoUsableContent
				        ? "concrete depots are not usable"
				        : cmResult == SourceResult::VirtualDlc
				            ? "DLC has no usable content depots"
				            : "local cache write failed");
				// A content verdict is a property of the app, not of this
				// attempt: repeating it costs a CM round-trip and returns the
				// same answer. A local write failure is NOT terminal in that
				// sense — the next pass may well succeed — so it stays
				// retryable.
				if (cmResult == SourceResult::NoUsableContent ||
				    cmResult == SourceResult::VirtualDlc)
					noteTerminalProvisionResult(appId, cmResult, cn, publication);
				if (cmResult == SourceResult::VirtualDlc)
					return ProvisionOutcome::NotApplicable;
				if (cmResult == SourceResult::NoUsableContent)
					return ProvisionOutcome::NoUsableContent;
				return cmResult == SourceResult::LocalFailure
				    ? ProvisionOutcome::LocalFailure
				    : ProvisionOutcome::IncompleteContent;
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
		g_pLog->info("AppInfoProvision: app=%u GET %s\n", appId, url.c_str());

		// Retry transient failures (timeouts, 5xx, cold-network DNS) with
		// a bounded linear backoff.  A single steamcmd.net timeout used to
		// leave the app unprovisioned for the whole session unless Steam
		// happened to re-exec setup(); the larger an app's product-info
		// JSON is, the more likely the 30s total-transfer timeout trips on
		// a slow first request (observed: Outlast 238320's 8-depot JSON
		// timed out while the smaller 2262770 succeeded in the same pass).
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

		// Use info, not warn: warn fires a critical notify-send popup
		// (CLog ctor configures urgency=critical for warn).  A single
		// provider exhausting its retries isn't user-actionable noise.
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

	std::string wire;
	bool synthesized = false;
	const SourceResult renderResult = renderAppinfoBuffer(
	    appNode, appId, wire, publication, &synthesized);
	if (renderResult != SourceResult::Success)
	{
		const char* reason = "invalid response";
		if (renderResult == SourceResult::IncompleteContent)
			reason = "response contains no concrete depot data";
		else if (renderResult == SourceResult::NoUsableContent)
			reason = "concrete depots are not usable";
		else if (renderResult == SourceResult::VirtualDlc)
			reason = "DLC has no usable content depots";
		g_pLog->info("AppInfoProvision: app=%u render stopped (%s)\n", appId, reason);
		if (renderResult == SourceResult::VirtualDlc ||
		    renderResult == SourceResult::NoUsableContent)
			noteTerminalProvisionResult(
				appId, renderResult, changeNumber, publication);
		if (renderResult == SourceResult::VirtualDlc)
			return ProvisionOutcome::NotApplicable;
		if (renderResult == SourceResult::NoUsableContent)
			return ProvisionOutcome::NoUsableContent;
		return renderResult == SourceResult::LocalFailure
		    ? ProvisionOutcome::LocalFailure
		    : ProvisionOutcome::IncompleteContent;
	}

	// Spot-check: the wire must contain the depots block, otherwise the
	// upstream JSON itself is stripped (rare, but happens for retired
	// titles).  Skip the splice in that case rather than persist a
	// useless entry.
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->info("AppInfoProvision: app=%u JSON has no depots, skipping\n", appId);
		return ProvisionOutcome::IncompleteContent;
	}

	// Manifest-GID pins are DELIBERATELY NOT applied to the provisioned wire
	// buffer.
	//
	// We used to rewrite every provisioned buffer's public gid to the
	// pinned gid.  That is structurally defeated by Steam: when the user
	// clicks Install, Steam issues a `RequestAppInfoUpdate` that
	// downloads fresh product-info over HTTP and OVERWRITES our
	// appinfo.vdf entry with the live public gid (confirmed in
	// appinfo_log.txt).  Steam then plans the install with the live
	// public gid, not our pin.  So pinning the provisioned buffer only
	// caused a mismatch: we pre-staged the pinned manifest, Steam asked
	// for the live one, BYldRequestDepotManifest got called and returned
	// "Access Denied" -> first-attempt "No connection".
	//
	// Depot decryption keys are per-DEPOT, not per-manifest, so the live
	// public build decrypts and installs fine with the same key (verified
	// on the VM: Gang Beasts depot 285903 committed successfully with the
	// live gid).  Provisioning the live public gid therefore means the
	// gid we pre-stage in PICS recv == the gid Steam requests == BYld is
	// skipped == first-attempt install succeeds.
	//
	// ManifestPins are applied later, after Steam has built the plan:
	// BuildDepotDependency rewrites each DepotEntry using its AppId, and
	// ReconcilePin patches the in-memory TARGET vectors.  This applies to
	// every configured app-scoped pin.  lockedApps remains separate and only
	// controls update suppression in Apps::shouldDisableUpdates.

	// Compute sha[20] over the FINAL wire buffer (after prune).
	//
	// We must NOT reuse SteamCMD's `_sha`: that hash describes the
	// upstream, unmodified product-info, but we've dropped depots and
	// repointed manifest GIDs.  AppInfoVdf::injectApp treats
	// (appid, change_number, sha) as an idempotency key and skips the
	// rewrite when all three match an existing entry.  If we kept the
	// upstream sha, a previously-injected full-depot entry would never
	// be replaced by our pruned one — the on-disk appinfo.vdf would
	// keep stale depots and Steam would show 0 B.  Hashing our own
	// bytes guarantees the key changes whenever our output changes.
	std::string sha20;
	{
		std::uint8_t tmp[20];
		sha1BytesInternal(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	if (!persistBuffer(appId, changeNumber, sha20, wire, publication,
	                   synthesized))
	{
		g_pLog->info("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return ProvisionOutcome::LocalFailure;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return ProvisionOutcome::Updated;
}

bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath)
{
	ProvisionPassState pass;
	ProvisionPassContext context;
	return isProvisioned(provisionAppDetailed(
		appId, appinfoVdfPath, pass, context));
}

bool publishRuntimeAppInfo(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requested);

void enrichAndPublishDlcMetadata(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& publishedBases);

int provisionAppsPass(const std::string& appinfoVdfPath,
                       const std::unordered_set<uint32_t>& added,
                       bool onlyMissing, ProvisionPassContext& context,
	                       std::unordered_set<uint32_t>* fallbackApps,
	                       const char* origin)
{
	if (added.empty()) return 0;
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
	// through to the steamcmd.net HTTP chain inside provisionApp.  Skip
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
			if (!onlyMissing) primeTerminalMemo(appId);
			const bool terminalKnown = onlyMissing
				? observeTerminal(appId, 0).applies
				: terminalProvisionResultKnown(appId, 0, nullptr);
			if (terminalKnown)
				continue;
			if (onlyMissing)
			{
				// Runtime cold recovery already made a nonblocking readiness
				// decision on the callback thread. Do not turn that path back into
				// blocking cache validation here.
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
			: terminalProvisionResultKnown(appId, observedChange, &outcome);
		if (!terminalKnown)
			outcome = provisionAppDetailed(appId, appinfoVdfPath, pass, context);
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
			g_pLog->notifyUser(UserMsg::GamePreparationFailed,
			                   std::to_string(appId));
		}
		else if (notice == ProvisionNotice::LocalStorage)
		{
			g_pLog->notifyUser(UserMsg::LocalStorageError);
		}
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

int provisionApps(const std::string& appinfoVdfPath,
                  const std::unordered_set<uint32_t>& added,
                  bool onlyMissing)
{
	if (added.empty()) return 0;

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto addedSnapshot = coordinator.snapshot([&] { return added; });
	if (addedSnapshot.empty()) return 0;

	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, addedSnapshot, onlyMissing, context, nullptr, "scoped");
	coordinator.commit([&] {
		persistPendingProtonMappings(false);
	});
	return provisioned;
}

int provisionAllAddedApps(const std::string& appinfoVdfPath,
                          bool allowConfigWrite)
{
	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto added = coordinator.snapshot(
		[] { return g_config.managedAppIds.get(); });
	if (added.empty()) return 0;

	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, added, false, context, nullptr, "startup");
	coordinator.commit([&] {
		if (allowConfigWrite)
		{
			if (!injectProtonMappings())
				persistPendingProtonMappings(true);
		}
		else
		{
			persistPendingProtonMappings(false);
		}
	});
	return provisioned;
}

ProvisionPassSummary provisionRequestedApps(
	const std::string& appinfoVdfPath,
	const std::vector<RefreshRequest>& requests,
	bool allowConfigWrite)
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
	fetch.reserve(accepted.size());
	std::size_t busy = 0;
	for (const RefreshRequest& request : accepted)
	{
		const CacheProbe probe = probeCache(
			request.appId, CacheProbeMode::BlockingValidate);
		if (probe.readiness == CacheReadiness::Busy) ++busy;
		if (!requestNeedsFetch(request, probe.readiness, probe.changeNumber))
		{
			++summary.ready;
			continue;
		}
		if (terminalProvisionResultKnown(
			request.appId, request.minimumChangeNumber, nullptr))
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

	std::vector<RefreshRequest> publish;
	for (const RefreshRequest& request : fetch)
	{
		ProvisionOutcome outcome = ProvisionOutcome::IncompleteContent;
		const auto change = context.cmChanges.find(request.appId);
		const uint32_t observed = change == context.cmChanges.end()
			? request.minimumChangeNumber : change->second;
		if (!terminalProvisionResultKnown(request.appId, observed, &outcome))
			outcome = provisionAppDetailed(
				request.appId, appinfoVdfPath, pass, context,
				request.forceRefresh);
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

	bool runtimePublished = false;
	coordinator.commit([&] {
		if (allowConfigWrite)
		{
			if (!injectProtonMappings()) persistPendingProtonMappings(true);
		}
		else persistPendingProtonMappings(false);
		std::vector<RefreshRequest> basePublish;
		for (const auto& request : publish)
		{
			if ((request.reasons & ~reasonMask(RefreshReason::DlcMetadata)) != 0)
				basePublish.push_back(request);
		}
		if (!basePublish.empty())
			runtimePublished = publishRuntimeAppInfo(appinfoVdfPath, basePublish);
	});
	const bool metadataOnly = std::any_of(
		publish.begin(), publish.end(), [](const RefreshRequest& request) {
			return (request.reasons & reasonMask(RefreshReason::DlcMetadata)) != 0;
		});
	if (runtimePublished || metadataOnly)
		enrichAndPublishDlcMetadata(appinfoVdfPath, publish);
	g_pLog->info(
		"AppInfoProvision: pass origin=targeted requested=%zu fetched=%zu "
		"updated=%zu ready=%zu terminal=%zu failed=%zu\n",
		summary.requested, summary.fetched, summary.updated,
		summary.ready + summary.fallback, summary.terminal, summary.failed);
	return summary;
}

int provisionColdStartApps(const std::string& appinfoVdfPath,
                           const std::unordered_set<uint32_t>& candidates,
                           ColdStartMode mode,
                           std::unordered_set<uint32_t>* sanitizedApps,
                           bool allowConfigWrite)
{
	if (sanitizedApps) sanitizedApps->clear();
	std::unordered_set<uint32_t> fallbackApps;

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto managedApps = coordinator.snapshot([&] {
		const auto managed = g_config.managedAppIds.get();
		std::unordered_set<uint32_t> scoped;
		for (const uint32_t appId : candidates)
			if (appId != 0 && managed.count(appId) != 0) scoped.insert(appId);
		return scoped;
	});
	// Disk-aware terminal priming is allowed here only during startup. Build
	// the manifest observation index once for the whole fleet; rebuilding it
	// inside terminalProvisionResultKnown() for every app made a network-free
	// startup scale with apps × archived manifests. Runtime callbacks remain
	// strictly memo-only.
	if (coldStartPrimesTerminalMemo(mode))
	{
		const auto archivedGids = ManifestStore::archivedGidIndex();
		for (const uint32_t appId : managedApps)
		{
			const std::string fingerprint =
				localContentFingerprint(appId, archivedGids);
			primeTerminalMemo(appId, fingerprint);
		}
	}
	// Startup validates persisted pairs before accepting them for the splice;
	// the PICS callback reads only its memo while the user is interacting.
	// Both paths recover only missing or invalid pairs; a validated stale pair
	// is already usable and must not trigger synchronous provider work.
	std::vector<std::pair<uint32_t, CacheReadiness>> probed;
	probed.reserve(managedApps.size());
	std::size_t skippedTerminal = 0;
	for (const uint32_t appId : managedApps)
	{
		const CacheProbeMode probeMode = mode == ColdStartMode::StartupRequireUsablePair
			? CacheProbeMode::BlockingValidate
			: CacheProbeMode::NonBlockingMemoOnly;
		const CacheProbe cacheProbe = probeCache(appId, probeMode);
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
	std::unordered_set<uint32_t> cold = selectColdStartApps(probed, mode);
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
	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, cold, true, context, &fallbackApps,
		allowConfigWrite ? "startup-cold" : "runtime-cold");
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
			if (sanitizedApps &&
			    shouldMarkColdCacheSanitized(explicitFallback))
				sanitizedApps->insert(appId);
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
		if (sanitizedApps &&
		    shouldMarkColdCacheSanitized(explicitFallback))
			sanitizedApps->insert(appId);
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
		if (allowConfigWrite)
		{
			if (!injectProtonMappings())
				persistPendingProtonMappings(true);
		}
		else
		{
			persistPendingProtonMappings(false);
		}
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
	const auto loaded = loadPendingProtonMappings();
	if (!loaded.lockAcquired || loaded.status == PendingProtonFileStatus::Invalid)
	{
		// A lock/read/parse failure is not evidence that the file is empty.
		// Leave both the on-disk mappings and any in-memory retry state intact.
		return;
	}
	if (loaded.status == PendingProtonFileStatus::Missing)
		return;
	if (loaded.ids.empty())
	{
		// This also removes entries left behind by a previous config removal;
		// the valid file was filtered against current management above.
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
		return;
	}
	g_needProton.insert(loaded.ids.begin(), loaded.ids.end());
	if (injectProtonMappings())
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
}

enum class RefreshWorkerStartResult
{
	Started,
	NotStarted,
	Uncertain,
};

bool publishRuntimeAppInfo(
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
		return false;

	const std::unordered_set<std::uint32_t> scoped(
		selected.begin(), selected.end());
	if (AppInfoVdf::injectCachedApps(appinfoVdfPath, scoped) == 0)
	{
		g_pLog->warn(
			"AppInfoProvision: live appinfo splice failed for %zu app(s); "
			"restart remains available\n",
			selected.size());
		return false;
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
			return false;
		case AppInfoReload::Status::ReadFailed:
			g_pLog->warn(
				"AppInfoProvision: Steam rejected live appinfo disk reload for %zu "
				"app(s); restart remains available\n",
				selected.size());
			return false;
		case AppInfoReload::Status::Loaded:
			if (!AppInfoReload::allRequestedPresent(reloaded, selected.size()))
			{
				g_pLog->warn(
					"AppInfoProvision: live appinfo reload found %zu/%zu current "
					"app(s); restart remains available\n",
					reloaded.present, selected.size());
				return false;
			}
			else
			{
				g_pLog->info(
					"AppInfoProvision: published %zu app(s) into the live "
					"appinfo cache\n",
					reloaded.present);
			}
			return true;
	}
	return false;
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
		if (!guard.cacheLock->acquired()) return false;
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
		sha1BytesInternal(baseWire.data(), baseWire.size(), baseDigest);
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
			sha1BytesInternal(normalized.data(), normalized.size(), digest);
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
		if (!cacheLock.acquired()) return;
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
	if (complete) Apps::setAddedAppDlcIds(allDlc.appDlc);
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

RefreshWorkerStartResult startRefreshWorker(
    const std::string& appinfoVdfPath, std::uint64_t token,
    const std::vector<RefreshRequest>& requests)
{
	bool workerMayStillExist = false;
	const bool started = ThreadStart::startDetached(
		[path = appinfoVdfPath, token, requests]
		{
			ThreadStart::runGuarded(
				[path, requests]
				{
					ScopedNotifySuppression suppression;
					BootProf::Span profile(g_pLog.get(), "provision.async");
					(void)provisionRequestedApps(path, requests, false);
				},
				[]
				{
					if (g_pLog)
						g_pLog->warn(
						    "AppInfoProvision: async refresh worker failed; will retry\n");
				},
				[path, token] { finishRefresh(path, token); });
		},
		[] { refreshWorkerStartFailed(); },
		ThreadStart::NoopDetachFailure{},
		ThreadStart::StdThreadDetacher{},
		ThreadStart::StdThreadJoiner{},
		[&workerMayStillExist] {
			workerMayStillExist = true;
			refreshWorkerStartFailed();
		});
	if (started)
		return RefreshWorkerStartResult::Started;
	if (workerMayStillExist)
		return RefreshWorkerStartResult::Uncertain;

	return RefreshWorkerStartResult::NotStarted;
}

void retainRefreshAfterFailedStart(std::uint64_t token,
                                   RefreshWorkerStartResult result,
                                   const std::vector<RefreshRequest>& requests)
{
	if (result != RefreshWorkerStartResult::NotStarted ||
	    !shouldRequeueRefreshAfterStartFailure(/*workerMayStillExist=*/false))
		return;

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

	const auto startResult = startRefreshWorker(nextPath, nextToken, nextRequests);
	retainRefreshAfterFailedStart(nextToken, startResult, nextRequests);
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

	const auto startResult = startRefreshWorker(appinfoVdfPath, token, batch);
	retainRefreshAfterFailedStart(token, startResult, batch);
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
	if (!cacheLock.acquired())
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
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot rejected cache pair for app=%u: %s\n",
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
	}

	const auto cacheDir = getCacheDir();
	bool pendingMappingRemoved = false;
	bool complete = false;
	{
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (!cacheLock.acquired()) return false;
		ProvisionTerminal::Store(cacheDir).erase(appId);
		// Decide marker retention only after both publication and cache locks are
		// held. A concurrent synthetic publication must be visible here before
		// the cleanup chooses whether its protection marker is preserved.
		const bool preserveSyntheticMarker =
			cache::shouldPreserveSyntheticMarker(
				preserveTicketArtifacts,
				preserveTicketArtifacts && isSynthesizedAppLocked(appId));
		pendingMappingRemoved = removePendingProtonMappingLocked(appId);

		const auto relatedDepots = ManifestId::getExclusiveDepotsForApp(appId);
		const std::string suffix =
			".forgotten." +
			std::to_string(static_cast<long long>(std::time(nullptr))) + "." +
			std::to_string(static_cast<long long>(::getpid()));
		const auto records = SynthMark::quarantineAppArtifacts(
			cacheDir, appId, relatedDepots, suffix,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		for (const auto& record : records)
		{
			g_pLog->infoOnce("AppInfoProvision: quarantined removed-app cache %s -> %s\n",
			                record.original.string().c_str(),
			                record.quarantined.string().c_str());
		}
		complete = !SynthMark::hasAppArtifacts(
			cacheDir, appId, relatedDepots,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		if (complete)
		{
			// Drop this app's relation only after every source artifact has been
			// moved. A partial quarantine remains retryable with the same depot
			// ownership information, while shared depots stay in the catalog.
			ManifestId::forgetApp(appId);
		}
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
