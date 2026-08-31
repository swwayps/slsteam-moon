// SPDX-License-Identifier: AGPL-3.0-only

#include "prewarm.hpp"

#include "appinfo_provision.hpp"

#include "depotkey.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../thread_start.hpp"

#include "../utils/ManifestFetch.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <set>
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
std::atomic<bool> g_stopRequested{false};
std::mutex g_stopMu;
std::condition_variable g_stopCv;

template <typename Rep, typename Period>
bool waitOrStop(std::chrono::duration<Rep, Period> delay)
{
	std::unique_lock<std::mutex> lock(g_stopMu);
	return g_stopCv.wait_for(lock, delay, []
	{
		return g_stopRequested.load(std::memory_order_acquire);
	});
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
	std::string out;
	if (!AppInfoProvision::readValidatedCacheBuffer(appId, out)) return {};
	return out;
}

void runLoop()
{
	using namespace std::chrono_literals;

	// Small gap between depots so a cold first pass doesn't fire every CDN
	// request at once. We block on our OWN thread, so this just paces us.
	constexpr auto kPerDepotGap = 200ms;

	// This is our dedicated, session-long worker thread.  Suppress desktop
	// popups for it: a re-stage failure here is background self-healing, not
	// a user-actionable event, and a big multi-DLC title (Steam purges its
	// unused DLC depots after the base commit) would otherwise fire a popup
	// per failing depot per 30s pass.  The warnings still reach ~/.SLSsteam
	// .log; the synchronous install path (a different thread) keeps popups.
	t_suppressNotify = true;

	// Persist across passes: a depot that stays inaccessible cools down after
	// kMaxFails passes. It is re-admitted after the bounded slow interval so a
	// provider recovery can unblock a waiting install without a tight loop.
	constexpr int kMaxFails = 3;
	FailureTracker failures(kMaxFails);
	PassBackoff backoff;

	for (;;)
	{
		if (g_stopRequested.load(std::memory_order_acquire))
			return;
		std::vector<DepotGid> targets;
		bool newTarget = false;
		bool hasEligibleTarget = false;
		const auto added = g_config.addedAppIds.get();
		if (!added.empty())
		{
			std::set<DepotGid> seenTargets;
			auto appendTarget = [&targets, &seenTargets](const DepotGid& target)
			{
				const auto [depotId, gid] = target;
				if (depotId && gid && seenTargets.insert(target).second)
					targets.push_back(target);
			};

			const auto hasKey = [](uint32_t depotId) {
				return !DepotKey::getCachedKey(depotId).key.empty();
			};
			for (uint32_t appId : added)
			{
				const auto pins = g_config.getAppPinnedDepots(appId);
				std::string buf = readBuffer(appId);
				if (!pins.empty())
				{
					// A partially pinned Lua still installs its other depots at the
					// public gid. Include those exact public targets too; otherwise a
					// locally-ready pin could hide a missing sibling manifest.
					const auto publicTargets = buf.empty()
					    ? std::vector<DepotGid>{}
					    : planStageTargets({buf}, hasKey);
					for (const auto& target : planPinnedStageTargets(publicTargets, pins))
						appendTarget(target);
				}
				else if (!buf.empty())
				{
					for (const auto& target : planStageTargets({buf}, hasKey))
						appendTarget(target);
				}
			}

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
					const auto before = targets.size();
					appendTarget(wm);
					if (targets.size() != before) ++workshopCount;
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
				// A repeatedly inaccessible target waits for the bounded cooldown
				// below before it is admitted for another real attempt.
				if (failures.isBlacklisted(depotId, gid)) continue;
				hasEligibleTarget = true;

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
						    "failed passes (cooling down)\n",
						    depotId, static_cast<unsigned long long>(gid),
						    kMaxFails);
					}
				}
				if (waitOrStop(kPerDepotGap))
					return;
			}

		}

		newTarget = backoff.observeTargets(targets);
		const bool noOpPass = !targets.empty() && !hasEligibleTarget && !newTarget;
		backoff.recordPass(noOpPass);
		if (noOpPass && backoff.noOpPasses() == PassBackoff::kNoOpPassesBeforeBackoff)
		{
			g_pLog->debug(
			    "Prewarm: all targets cooling down; retrying within 1 minute\n");
			failures.resetAll();
		}
		if (waitOrStop(backoff.interval()))
			return;
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

	g_stopRequested.store(false, std::memory_order_release);
	g_pLog->info("Prewarm: starting background manifest pre-warm worker\n");

	// Detached: lives for the Steam session.  MUST only be reached from a
	// real Steam worker thread (the PICS recv path) — never the LD_AUDIT
	// load()/setup() path, as that path must not spawn background threads.
	// Keep the std::thread object owned by startDetached until detach succeeds;
	// if it fails, request a prompt stop so the recovery join cannot hang on
	// the session-long loop.
	const bool started = ThreadStart::startDetached(
		[]
		{
			ThreadStart::runGuarded(
				[] { runLoop(); },
				[]
				{
					g_pLog->warn("Prewarm: background worker failed unexpectedly; will retry\n");
				},
				[]
				{
					g_started.store(false, std::memory_order_release);
				});
		},
		[]
		{
			g_stopRequested.store(false, std::memory_order_release);
			g_started.store(false, std::memory_order_release);
		},
		[]
		{
			g_stopRequested.store(true, std::memory_order_release);
			g_stopCv.notify_all();
		});
	if (!started)
	{
		g_pLog->warn("Prewarm: unable to start background worker; will retry\n");
	}
}

} // namespace Prewarm
