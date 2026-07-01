// SPDX-License-Identifier: AGPL-3.0-only
//
// See appinfo_provision.hpp for design notes.

#include "appinfo_provision.hpp"

#include "cmclient.hpp"
#include "depotkey.hpp"
#include "dlcids.hpp"
#include "manifestid.hpp"
#include "manifeststore.hpp"
#include "manifestsynth.hpp"
#include "provision_cache.hpp"
#include "retry.hpp"
#include "synthmark.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include "../utils/ManifestFetch.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/yaml.h"
#include "yaml-cpp/emitter.h"

#include <openssl/sha.h>

#include <curl/curl.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
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

bool httpGetJson(const std::string& url, std::string& body, std::string& diag)
{
	if (!loadCurl()) { diag = "libcurl unavailable"; return false; }
	CURL* c = p_curl_easy_init();
	if (!c) { diag = "curl_easy_init failed"; return false; }

	body.clear();
	p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	// Bounded timeouts.  AppInfoProvision runs in setup() on the startup
	// path, so we must not stall Steam's launch for too long if
	// steamcmd.net is slow or unreachable.  The fetch is now wrapped in a
	// bounded retry (see provisionApp), so each individual attempt can be
	// tighter: a genuinely down host fails fast at connect (8s) instead of
	// burning the full transfer budget, while a transient slow transfer
	// still gets a generous 20s and is retried with backoff.  Worst case
	// per app ≈ 3*20s + (1s+2s) backoff ≈ 63s only if every attempt times
	// out at the transfer stage; an unreachable host is ≈ 3*8s + 3s.
	p_curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
	// Same multi-thread safety justification as ManifestFetch::httpGet.
	p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-AppInfoProvision/0.1");

	const CURLcode rc = p_curl_easy_perform(c);
	long status = 0;
	if (p_curl_easy_getinfo) p_curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	p_curl_easy_cleanup(c);

	if (rc != CURLE_OK)
	{
		diag = p_curl_easy_strerror ? p_curl_easy_strerror(rc) : "curl error";
		return false;
	}
	if (status != 200)
	{
		std::stringstream s; s << "HTTP " << status;
		diag = s.str();
		return false;
	}
	if (body.empty()) { diag = "empty body"; return false; }
	return true;
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
//   - If the surviving set has at least one playable depot but the
//     original "common.oslist" included an OS we just dropped, narrow
//     "oslist" to the OSes we still have so Steam picks Proton (for
//     "windows") rather than a phantom native binary.
//
// Mutates `body` in place.  `appId` is for log lines only.
void pruneUnsupportedDepots(YAML::Node& body, uint32_t appId)
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

		// DLC entries have no `manifests` block; they're virtual and
		// don't need a decryption key — keep them.
		const bool isDlc = depotNode.IsMap() && depotNode["dlcappid"] &&
		                   !depotNode["manifests"];
		const auto savedKey = DepotKey::getCachedKey(depotId);
		const bool hasKey = !savedKey.key.empty();

		if (!hasKey && !isDlc)
		{
			++dropped;
			continue;  // omit from newDepots
		}
		++kept;
		newDepots[key] = YAML::Clone(depotNode);

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

	if (dropped == 0)
	{
		// Even when we drop nothing, the app may be natively
		// non-Linux (e.g. the user added a windows-only title).  Mark
		// it for Proton when no surviving depot targets Linux.
		if (!survivingOs.empty() && !survivingOs.count("linux"))
		{
			g_needProton.insert(appId);
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
		g_needProton.insert(appId);
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
std::string getCacheDir();

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
				ManifestSynth::parseManifestSizes(bytes, size, download);
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

		// Mark the app synthetic so the outgoing-PICS hook strips it from
		// Steam's product-info requests.  Without this, Steam's runtime
		// RequestAppInfoUpdate returns an EMPTY buffer (token denied) and
		// clobbers these synthesized depots in memory -> install dialog drops
		// to 0 B / "Invalid install path".  Persisted: the surviving setup()
		// pass may hit the provisioning cache and skip synthesis, but the
		// marker from the cold pass remains.
		SynthMark::mark(getCacheDir(), appId);
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

// Render the SteamCMD-style JSON object for one app into the wire-text
// VDF format that AppInfoVdf::translateWireToIndexed accepts.  Returns
// true on success.
bool renderAppinfoBuffer(const YAML::Node& appNode, uint32_t appId, std::string& wireOut)
{
	if (!appNode.IsMap()) return false;

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
	if (!body.IsMap() || body.size() == 0) return false;

	// Token-locked apps (product-info access token denied to anonymous
	// sessions) arrive with NO depots.  Rebuild the block from the depot
	// key + archived manifest we already hold, so the prune/Proton/splice
	// tail below runs unchanged.  No-op when real depots are present.
	if (YAML::Node d = body["depots"]; !d || !d.IsMap() || d.size() == 0)
	{
		const int n = synthesizeDepotsFromStore(body, appId);
		if (n > 0)
			g_pLog->info("AppInfoProvision: app=%u synthesized %d depot(s) from "
			             "stored manifests (product-info had none)\n", appId, n);
	}

	// Strip depots we can't decrypt; narrow common.oslist accordingly.
	// Done here (post-envelope-strip, pre-emit) so the output Steam
	// reads is consistent and the change is invisible to Steam beyond
	// "the user only owns the windows depot".
	pruneUnsupportedDepots(body, appId);

	// Clear the launch-time legacy CD-key gate (see helper above): without
	// this the GettingLegacyKey step fails AccessDenied for an unowned app
	// and the launch aborts before the game/Proton ever starts.
	neutralizeLegacyCdKey(body, appId);

	// Steam's appinfo wire format wraps the document in "appinfo" { ... }.
	wireOut.clear();
	wireOut.append("\"appinfo\"\n{\n");
	emitNode(wireOut, body, 1);
	wireOut.append("}\n");
	return true;
}

// ---------------------------------------------------------------------------
// On-disk cache (mirrors feats/pics.cpp layout so AppInfoVdf::injectAllCached
// picks the buffers up at next start).
// ---------------------------------------------------------------------------

std::string getCacheDir()
{
	const std::string dir = g_config.getDir() + "/cache";
	if (!std::filesystem::exists(dir))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
	}
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

// Return the last-modified time (epoch seconds) of `appId`'s on-disk
// provisioned buffer, and whether it exists and is non-empty.  Used by
// provisionApp's short-lived cache to skip the network fetch during the
// setup() re-exec storm of a single boot.
bool statBuffer(uint32_t appId, long long& mtimeSecsOut)
{
	struct stat st{};
	if (stat(getBufferPath(appId).c_str(), &st) != 0) return false;
	if (st.st_size <= 0) return false;
	mtimeSecsOut = static_cast<long long>(st.st_mtime);
	return true;
}

// Freshness window for the provisioning cache, in seconds.  Short by
// design: it must cover Steam's setup() re-exec storm within one boot
// (so the 8-app fleet is fetched once, not once per pass) without
// surviving into a later genuine relaunch, where we re-fetch the live
// public gid (see provision_cache.hpp for the gid-staleness rationale).
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
                   const std::string& sha20, const std::string& wire)
{
	if (sha20.size() != 20)
	{
		g_pLog->debug("AppInfoProvision: refuse to persist app=%u, sha size %zu\n",
		              appId, sha20.size());
		return false;
	}
	const auto bufPath  = getBufferPath(appId);
	const auto metaPath = getMetaPath(appId);

	{
		std::ofstream ofs(bufPath, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open()) return false;
		ofs.write(wire.data(), static_cast<std::streamsize>(wire.size()));
	}
	{
		YAML::Emitter em;
		em << YAML::BeginMap;
		em << YAML::Key << "appid"         << YAML::Value << appId;
		em << YAML::Key << "change_number" << YAML::Value << changeNumber;
		em << YAML::Key << "wire_size"     << YAML::Value << wire.size();
		em << YAML::Key << "sha_b64"       << YAML::Value << base64::to_base64(sha20);
		em << YAML::EndMap;
		std::ofstream ofs(metaPath, std::ios::trunc);
		if (!ofs.is_open()) return false;
		ofs.write(em.c_str(), em.size());
	}
	return true;
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
	// bytes to be a real entry header.  Per .kiro/research/appinfo-vdf-
	// format/README.md, header layout is:
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

void sha1Bytes(const void* data, std::size_t n, std::uint8_t out[20])
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

// Inject a `CompatToolMapping` entry for each app in g_needProton into
// `config/config.vdf`, so Steam runs them through Proton.  Best-effort,
// text-level edit (same approach as DepotKey::disableShaderCache).  Only
// adds entries that are missing; never overwrites a user's existing
// choice.
void injectProtonMappings()
{
	if (g_needProton.empty()) return;
	const auto root = steamRootForConfig();
	if (root.empty()) return;
	const auto path = root + "/config/config.vdf";
	if (!std::filesystem::exists(path)) return;

	std::string content;
	{
		std::ifstream ifs(path);
		if (!ifs.is_open()) return;
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
		if (steamPos == std::string::npos) return;
		const auto steamBrace = content.find('{', steamPos);
		if (steamBrace == std::string::npos) return;
		const std::string block =
			"\n\t\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t\t{\n\t\t\t\t\t}";
		content.insert(steamBrace + 1, block);
		mapPos = content.find("\"CompatToolMapping\"");
		mapBrace = content.find('{', mapPos);
	}
	if (mapBrace == std::string::npos) return;

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
			"\t\t\t\t\t\t\t\"name\"\t\t\"proton_experimental\"\n"
			"\t\t\t\t\t\t\t\"config\"\t\t\"\"\n"
			"\t\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
			"\t\t\t\t\t\t}";
		content.insert(mapBrace + 1, entry);
		++added;
	}

	if (added == 0) return;

	{
		std::ofstream ofs(path, std::ios::trunc);
		if (!ofs.is_open()) return;
		ofs << content;
	}
	g_pLog->infoOnce("AppInfoProvision: injected %d Proton CompatToolMapping entr%s into config.vdf\n",
	             added, added == 1 ? "y" : "ies");
}

// ---------------------------------------------------------------------------
// Native CM provider: parse the wire-text VDF buffer the anonymous CM
// returns into the same YAML::Node shape extractAppNode produces from
// steamcmd's JSON, so it flows through the identical prune/render/persist
// path.  The CM buffer is `"appinfo" { ... }` KV1 text (verified live);
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

// Render+prune+sha+persist a parsed appinfo node (shared tail used by
// both the CM and steamcmd paths).  Returns true if a buffer was
// written.  `changeNumber` is the PICS/JSON change number for the meta
// record.
bool renderAndPersist(uint32_t appId, const YAML::Node& appNode,
                      uint32_t changeNumber)
{
	std::string wire;
	if (!renderAppinfoBuffer(appNode, appId, wire))
	{
		g_pLog->warn("AppInfoProvision: app=%u render failed (likely empty body)\n", appId);
		return false;
	}
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->warn("AppInfoProvision: app=%u buffer has no depots, skipping\n", appId);
		return false;
	}

	std::string sha20;
	{
		std::uint8_t tmp[20];
		sha1Bytes(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	if (!persistBuffer(appId, changeNumber, sha20, wire))
	{
		g_pLog->warn("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return false;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return true;
}

// Provision one app from a native-CM wire buffer.  Returns true on
// success (buffer persisted).  Mirrors the steamcmd path's tail but skips
// the JSON parse — the CM buffer is already wire-text VDF.
bool provisionAppFromCmBuffer(uint32_t appId, const std::string& cmWire,
                              uint32_t changeNumber)
{
	if (cmWire.empty()) return false;
	CmVdfReader reader(cmWire.data(), cmWire.data() + cmWire.size());
	YAML::Node appNode = reader.parseAppinfo();
	if (!appNode || !appNode.IsMap() || appNode.size() == 0)
	{
		g_pLog->info("AppInfoProvision: app=%u CM buffer parse failed, fallback\n", appId);
		return false;
	}
	return renderAndPersist(appId, appNode, changeNumber);
}

// Batch map populated once per provisionAllAddedApps pass: appid -> CM
// wire buffer, and appid -> change number.  Consumed by provisionApp.
std::unordered_map<uint32_t, std::string> g_cmBuffers;
std::unordered_map<uint32_t, uint32_t>    g_cmChanges;


} // namespace


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath)
{
	(void)appinfoVdfPath;
	if (appId == 0) return false;

	// Short-lived on-disk cache to tame startup cost.  Steam re-execs
	// setup() several times during a single cold boot (observed 4x on
	// the Zorin VM), and each pass would otherwise issue one synchronous
	// HTTP GET per AddedApp — so the boot cost grew O(n_apps * n_passes)
	// and stalled Steam's launch the more games the user added.
	//
	// If we already wrote picsbuffer_<appid>.bin within the (short) TTL,
	// reuse it and skip the network: the buffer the earlier pass produced
	// reflects the SAME live state (DepotKeys; pins are disabled), so the
	// AppInfoVdf splice — idempotent on (appid, change, sha) — is a no-op
	// the second time anyway.  The TTL is deliberately short so a genuine
	// relaunch (minutes/hours later, > TTL) re-fetches the live public
	// gid; we must NOT serve a stale cross-session buffer, or we'd
	// reintroduce the staged-gid vs requested-gid mismatch the
	// install-first-attempt fix resolved.
	{
		const long long ttl = provisionTtlSecs();
		long long mtime = 0;
		const bool present = statBuffer(appId, mtime);
		const long long now = static_cast<long long>(std::time(nullptr));
		if (cache::isBufferReusable(present, mtime, now, ttl))
		{
			g_pLog->debug("AppInfoProvision: app=%u reusing cached buffer (age<%llds)\n",
			              appId, ttl);
			return true;
		}
	}

	// Native CM batch result (fetched once per provisionAllAddedApps pass,
	// directly from Valve — the PRIMARY source).  Falls through to the
	// steamcmd.net HTTP chain below if this app wasn't in the batch (CM
	// failed, or it was provisioned individually).
	{
		auto it = g_cmBuffers.find(appId);
		if (it != g_cmBuffers.end())
		{
			uint32_t cn = 0;
			if (auto ci = g_cmChanges.find(appId); ci != g_cmChanges.end())
				cn = ci->second;
			if (provisionAppFromCmBuffer(appId, it->second, cn))
			{
				g_pLog->info("AppInfoProvision: app=%u provisioned via CM\n", appId);
				return true;
			}
			g_pLog->info("AppInfoProvision: app=%u CM buffer unusable, trying steamcmd\n", appId);
		}
	}

	std::string body, diag;
	std::string url;
	bool fetched = false;
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
		const bool ok = retryWithBackoff(
			[&] { return httpGetJson(url, body, diag); },
			/*maxAttempts=*/3, /*baseDelayMs=*/1000,
			[](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
		if (ok) { fetched = true; break; }

		// Use info, not warn: warn fires a critical notify-send popup
		// (CLog ctor configures urgency=critical for warn).  A single
		// provider exhausting its retries isn't user-actionable noise.
		g_pLog->info("AppInfoProvision: app=%u provider failed after retries (%s), trying next\n",
		             appId, diag.c_str());
	}
	if (!fetched)
	{
		g_pLog->info("AppInfoProvision: app=%u all providers failed\n", appId);
		return false;
	}

	YAML::Node appNode;
	std::string err;
	if (!extractAppNode(body, appId, appNode, err))
	{
		g_pLog->warn("AppInfoProvision: app=%u parse failed: %s\n", appId, err.c_str());
		return false;
	}

	std::string wire;
	if (!renderAppinfoBuffer(appNode, appId, wire))
	{
		g_pLog->warn("AppInfoProvision: app=%u render failed (likely empty body)\n", appId);
		return false;
	}

	// Spot-check: the wire must contain the depots block, otherwise the
	// upstream JSON itself is stripped (rare, but happens for retired
	// titles).  Skip the splice in that case rather than persist a
	// useless entry.
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->warn("AppInfoProvision: app=%u JSON has no depots, skipping\n", appId);
		return false;
	}

	// Manifest-GID pins (`setManifestid` in the Lua) are DELIBERATELY NOT
	// applied here.
	//
	// We used to rewrite the provisioned buffer's public gid to the
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
	// (Honouring an explicit older-build pin would require keeping Steam
	// from refreshing appinfo.vdf at install time; tracked as a future
	// enhancement.  For the project goal — "the game installs and runs" —
	// the live public build is correct.)

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
		sha1Bytes(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	const uint32_t changeNumber = pickChangeNumber(appNode);

	if (!persistBuffer(appId, changeNumber, sha20, wire))
	{
		g_pLog->warn("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return false;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return true;
}

int provisionAllAddedApps(const std::string& appinfoVdfPath)
{
	const auto added = g_config.addedAppIds.get();
	if (added.empty()) return 0;

	// PRIMARY source: one batched anonymous-CM product-info request to
	// Valve for the whole fleet (≈0.3s for dozens of apps; replaces the
	// per-app steamcmd.net round-trips).  Best-effort: any miss falls
	// through to the steamcmd.net HTTP chain inside provisionApp.  Skip
	// only those apps whose buffer is still fresh on disk (the cache TTL
	// would short-circuit them anyway), so a warm relaunch makes no CM
	// request at all.  Disable entirely via SLSSTEAM_DISABLE_CM=1.
	g_cmBuffers.clear();
	g_cmChanges.clear();
	const bool cmDisabled = [] {
		const char* v = std::getenv("SLSSTEAM_DISABLE_CM");
		return v && *v && std::string(v) != "0";
	}();
	if (!cmDisabled)
	{
		std::vector<uint32_t> toFetch;
		const long long ttl = provisionTtlSecs();
		const long long now = static_cast<long long>(std::time(nullptr));
		for (uint32_t appId : added)
		{
			long long mtime = 0;
			const bool present = statBuffer(appId, mtime);
			if (!cache::isBufferReusable(present, mtime, now, ttl))
				toFetch.push_back(appId);
		}
		if (!toFetch.empty())
		{
			g_pLog->info("AppInfoProvision: fetching %zu app(s) via native CM\n",
			             toFetch.size());
			if (!CmClient::fetchProductInfo(toFetch, g_cmBuffers, &g_cmChanges))
			{
				g_cmBuffers.clear();
				g_cmChanges.clear();
				g_pLog->info("AppInfoProvision: native CM batch failed, using steamcmd fallback\n");
			}
		}
	}

	int provisioned = 0;
	for (uint32_t appId : added)
	{
		if (provisionApp(appId, appinfoVdfPath))
		{
			++provisioned;
		}
		else
		{
			// Terminal: both the CM batch and the steamcmd fallback failed
			// for this app, so it won't be installable this session. One
			// emit point here (not per-provider) avoids a false popup when
			// the CM path fails but steamcmd then succeeds. Throttled, so a
			// fleet-wide outage at startup collapses to a single popup.
			g_pLog->notifyUser(UserMsg::GamePreparationFailed);
		}
	}
	if (provisioned > 0)
	{
		g_pLog->info("AppInfoProvision: %d/%zu AdditionalApps provisioned\n",
		             provisioned, added.size());
	}

	// Drop the batch buffers; they can be large and are only needed for
	// this pass.
	g_cmBuffers.clear();
	g_cmChanges.clear();

	// Ensure windows-only AddedApps get a Proton CompatToolMapping so
	// Steam will download + run them on Linux.  Safe no-op if none.
	injectProtonMappings();

	return provisioned;
}

std::vector<uint32_t> collectDlcAppIdsForAddedApps()
{
	std::vector<uint32_t> out;
	std::unordered_set<uint32_t> seen;

	const auto added = g_config.addedAppIds.get();
	if (added.empty()) return out;

	for (uint32_t appId : added)
	{
		// Read the provisioned buffer we wrote in provisionApp().  Same
		// on-disk path feats/pics.cpp reads for synchronous staging.
		const auto path = getBufferPath(appId);
		std::ifstream ifs(path, std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) continue;

		const std::streamsize sz = ifs.tellg();
		if (sz <= 0 || sz > (64LL << 20)) continue;
		std::string wire;
		wire.resize(static_cast<std::size_t>(sz));
		ifs.seekg(0);
		ifs.read(wire.data(), sz);

		for (uint32_t dlcId : extractDlcAppIds(wire, appId))
		{
			// Never shadow a base AddedApp, and dedup across apps.
			if (added.count(dlcId)) continue;
			if (seen.insert(dlcId).second) out.push_back(dlcId);
		}
	}

	if (!out.empty())
	{
		g_pLog->info("AppInfoProvision: collected %zu DLC appid(s) from %zu AdditionalApps\n",
		             out.size(), added.size());
	}
	return out;
}

bool isSynthesizedApp(uint32_t appId)
{
	if (appId == 0) return false;
	return SynthMark::isMarked(getCacheDir(), appId);
}

} // namespace AppInfoProvision
