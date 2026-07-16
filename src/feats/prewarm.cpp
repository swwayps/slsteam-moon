// SPDX-License-Identifier: AGPL-3.0-only

#include "prewarm.hpp"

#include "depotkey.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include "../utils/ManifestFetch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace Prewarm
{

namespace
{

// Only ever flips false -> true.  The background worker runs for the rest
// of the Steam session, so once it is up we never start a second one.
std::atomic<bool> g_started{false};

std::string bufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache/picsbuffer_" << appId << ".bin";
	return ss.str();
}

// Resolve the Steam root (same candidate list ManifestFetch uses) so we
// can locate steamapps/workshop/appworkshop_<appid>.acf.
std::string findSteamRoot()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	static const char* suffixes[] = {
		"/.steam/steam",
		"/.steam/debian-installation",
		"/.local/share/Steam",
	};
	for (const char* suffix : suffixes)
	{
		const std::string candidate = std::string(home) + suffix;
		std::error_code ec;
		if (std::filesystem::exists(candidate + "/steam.sh", ec))
		{
			return candidate;
		}
	}
	return {};
}

// Read an AddedApp's workshop ACF, if present.  The workshop depot's
// per-item manifest gids are DYNAMIC and live only here (not in the
// provisioned picsbuffer), so the warm loop mines this separately.  Steam
// keeps the file under steamapps/workshop/appworkshop_<appid>.acf; some
// installs mirror it under both ~/.steam/steam and the debian-installation
// root, so we try the resolved root's path.
std::string readWorkshopAcf(const std::string& steamRoot, uint32_t appId)
{
	if (steamRoot.empty()) return {};
	const std::string path = steamRoot + "/steamapps/workshop/appworkshop_"
	                         + std::to_string(appId) + ".acf";
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (8LL << 20)) return {};
	std::string out;
	out.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	if (!ifs.read(out.data(), sz)) return {};
	return out;
}

// Read an AddedApp's provisioned appinfo buffer (written by
// AppInfoProvision at startup).  Same on-disk path feats/pics.cpp mines
// for synchronous install staging.
std::string readBuffer(uint32_t appId)
{
	const auto path = bufferPath(appId);
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (64LL << 20)) return {};
	std::string out;
	out.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	if (!ifs.read(out.data(), sz)) return {};
	return out;
}

void runLoop()
{
	using namespace std::chrono_literals;

	// Re-stage interval: short enough that a post-commit purge is healed
	// before the next planning pass (Steam's own auto-retry is ~30s), yet
	// not a busy loop.  A steady-state pass with everything already on disk
	// is just stat() calls — ManifestFetch's (gid,depotId) dedup returns the
	// cached success future after re-checking the file is still present, and
	// only re-fetches a manifest Steam actually purged.
	constexpr auto kPassInterval = 30s;
	// Small gap between depots so a cold first pass doesn't fire every CDN
	// request at once.  We block on our OWN thread, so this just paces us.
	constexpr auto kPerDepotGap = 200ms;

	// This is our dedicated, session-long worker thread.  Suppress desktop
	// popups for it: a re-stage failure here is background self-healing, not
	// a user-actionable event, and a big multi-DLC title (Steam purges its
	// unused DLC depots after the base commit) would otherwise fire a popup
	// per failing depot per 30s pass.  The warnings still reach ~/.SLSsteam
	// .log; the synchronous install path (a different thread) keeps popups.
	t_suppressNotify = true;

	// Persist across passes: a depot that stays inaccessible even after a
	// fresh request-code is dropped for the session after kMaxFails passes,
	// so we stop re-fetching (and re-logging) it every 30s forever.
	constexpr int kMaxFails = 3;
	FailureTracker failures(kMaxFails);

	for (;;)
	{
		const auto added = g_config.addedAppIds.get();
		if (!added.empty())
		{
			std::vector<std::string> buffers;
			buffers.reserve(added.size());
			for (uint32_t appId : added)
			{
				std::string buf = readBuffer(appId);
				if (!buf.empty())
				{
					buffers.push_back(std::move(buf));
				}
			}

			const auto hasKey = [](uint32_t depotId) {
				return !DepotKey::getCachedKey(depotId).key.empty();
			};
			auto targets = planStageTargets(buffers, hasKey);

			// Workshop depots are NOT in the provisioned picsbuffer's
			// `depots` block (the appid only appears as the value of
			// `workshopdepot`), and their per-item manifest gids are
			// DYNAMIC — they live in steamapps/workshop/appworkshop_<id>.acf.
			// So Steam's workshop update plans a depotId==appId manifest we
			// never staged -> BYldRequestDepotManifest -> 'Access Denied' ->
			// one ~30s retry (proven on the VM 2026-06-05, BoI 250900).
			// Mine the ACF and keep those manifests warm too, so the workshop
			// update finds the manifest on disk and skips BYld.
			//
			// LIMIT: a brand-new subscription's manifest isn't in the ACF
			// until after its first download, so the very first download of a
			// freshly-subscribed item still costs one retry; every subsequent
			// update / re-validate / relaunch is then first-attempt.
			const std::string steamRoot = findSteamRoot();
			std::size_t workshopCount = 0;
			for (uint32_t appId : added)
			{
				const std::string acf = readWorkshopAcf(steamRoot, appId);
				if (acf.empty()) continue;
				// The workshop depot id equals the appid; only warm it if we
				// hold that depot key (else the content can't decrypt/install
				// anyway and the fetch is wasted).
				if (!hasKey(appId)) continue;
				for (const auto& wm : extractWorkshopManifests(acf, appId))
				{
					targets.push_back(wm);
					++workshopCount;
				}
			}

			if (!targets.empty())
			{
				g_pLog->debug(
				    "Prewarm: keeping %zu manifest(s) warm across %zu AddedApp(s) "
				    "(incl. %zu workshop)\n",
				    targets.size(), added.size(), workshopCount);
			}

			for (const auto& [depotId, gid] : targets)
			{
				// Genuinely-inaccessible depot (delisted / region-locked /
				// gone from the CDN even with a fresh code): stop retrying it
				// for the rest of the session.
				if (failures.isBlacklisted(depotId, gid)) continue;

				// Blocking await ON OUR DEDICATED THREAD serialises the
				// fetches (no thread storm) and reuses ManifestFetch's
				// on-disk re-check: present -> returns instantly, purged ->
				// re-fetched so the next planning pass finds it and skips
				// BYldRequestDepotManifest entirely.
				const bool ready = ManifestFetch::awaitManifestBlob(
				    gid, depotId, ManifestFetch::getTimeoutSec());
				if (ready)
				{
					ManifestStore::archiveManifest(depotId, gid);
					ManifestStore::markPreferredGid(depotId, gid);
				}

				// Did the manifest actually land on disk?  awaitManifestBlob
				// returns its own status, but re-checking the file is the
				// ground truth and lets us drive the per-depot blacklist.
				if (!steamRoot.empty())
				{
					const std::string mpath = steamRoot + "/depotcache/"
					    + std::to_string(depotId) + "_"
					    + std::to_string(gid) + ".manifest";
					std::error_code ec;
					const auto sz = std::filesystem::file_size(mpath, ec);
					if (!ec && sz > 0)
					{
						failures.recordSuccess(depotId, gid);
					}
					else if (failures.recordFailure(depotId, gid))
					{
						g_pLog->debug(
						    "Prewarm: depot=%u gid=%llu blacklisted after %d "
						    "failed passes (skipping for this session)\n",
						    depotId, static_cast<unsigned long long>(gid),
						    kMaxFails);
					}
				}
				std::this_thread::sleep_for(kPerDepotGap);
			}
		}

		std::this_thread::sleep_for(kPassInterval);
	}
}

} // namespace

void ensureStarted()
{
	bool expected = false;
	if (!g_started.compare_exchange_strong(expected, true))
	{
		return; // already running
	}

	// No AddedApps -> nothing to warm.  Reset the flag so a later call
	// (after config / AddedApps are fully loaded) can still start it.
	if (g_config.addedAppIds.get().empty())
	{
		g_started.store(false);
		return;
	}

	g_pLog->info("Prewarm: starting background manifest pre-warm worker\n");

	// Detached: lives for the Steam session.  MUST only be reached from a
	// real Steam worker thread (the PICS recv path) — never the LD_AUDIT
	// load()/setup() path, as that path must not spawn background threads.
	std::thread(runLoop).detach();
}

} // namespace Prewarm
