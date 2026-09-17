#include "manifestdonor.hpp"

#include "manifestcode.hpp"
#include "manifestdonor_policy.hpp"
#include "stats_policy.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../sdk/CPackageInfo.hpp"
#include "../sdk/protobufs/steammessages_clientserver.pb.h"
#include "../thread_start.hpp"
#include "../utils/ManifestFetch.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ManifestDonor
{
namespace
{
	using Clock = std::chrono::steady_clock;
	struct ManifestKey
	{
		uint32_t depotId;
		uint64_t gid;
		bool operator==(const ManifestKey&) const = default;
	};
	struct ManifestKeyHash
	{
		std::size_t operator()(const ManifestKey& key) const noexcept
		{
			return std::hash<uint64_t>{}(key.gid)
			     ^ (std::hash<uint32_t>{}(key.depotId) << 1);
		}
	};
	std::mutex g_ownedMutex;
	std::unordered_map<uint32_t, PackageRecord> g_packages;
	std::unordered_set<uint32_t> g_licenses;
	std::unordered_set<uint32_t> g_ownedDepots;
	std::unordered_map<uint32_t, Wanted> g_seenDepots;
	bool g_licenseReady = false;
	std::atomic<uint64_t> g_generation{0};
	std::atomic<bool> g_running{false};
	std::mutex g_workerMutex;
	std::thread g_worker;
	bool g_stopping = false;
	std::mutex g_sleepMutex;
	std::condition_variable g_sleepCv;
	std::atomic<uint32_t> g_mintedThisSession{0};
	std::vector<Wanted> g_wanted;
	Clock::time_point g_wantedFetchedAt{};
	std::string g_wantedBase;
	bool g_wantedAvailable = false;
	struct CapturedCode
	{
		uint32_t depotId;
		uint64_t gid;
		uint64_t code;
		uint64_t generation;
		Clock::time_point capturedAt;
	};
	std::mutex g_captureMutex;
	std::vector<CapturedCode> g_capturedCodes;
	std::atomic<bool> g_forceWantedRefresh{false};
	std::atomic<int64_t> g_lastWantedAttemptMs{0};
	std::shared_ptr<std::atomic<bool>> g_sessionAlive =
		std::make_shared<std::atomic<bool>>(false);

	int64_t monotonicMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now().time_since_epoch()).count();
	}

	std::size_t rebuildOwnedLocked(
	    std::vector<std::pair<uint32_t, uint32_t>>& licensedApps)
	{
		auto derived = deriveLicensedPackages(g_licenses, g_packages);
		g_ownedDepots = std::move(derived.ownedDepots);
		licensedApps = std::move(derived.licensedApps);
		return derived.resolvedPackages;
	}

	void publishLicensedApps(
	    const std::vector<std::pair<uint32_t, uint32_t>>& licensedApps,
	    StatsPolicy::Context context)
	{
		std::vector<uint32_t> apps;
		apps.reserve(licensedApps.size());
		for (const auto& [packageId, appId] : licensedApps)
		{
			(void)packageId;
			apps.push_back(appId);
		}
		StatsPolicy::replaceNativePackageApps(context, apps);
	}

	std::string baseUrl(const CConfig::DonateSettings& cfg)
	{
		std::string base = cfg.url;
		while (base.size() > 8 && base.back() == '/') base.pop_back();
		return base;
	}

	void submit(const CConfig::DonateSettings& cfg, const std::string& body,
	            std::size_t count, uint64_t generation,
	            const std::shared_ptr<std::atomic<bool>>& sessionAlive)
	{
		if (body.empty() || !g_running || !sessionAlive ||
		    !sessionAlive->load(std::memory_order_acquire) ||
		    g_generation.load(std::memory_order_acquire) != generation ||
		    !g_config.donate.get().enabled) return;
		const auto response = ManifestFetch::archiveRequest(
		    "POST", baseUrl(cfg) + "/manifestcode/submit", body,
		    64u * 1024u, 15000, sessionAlive.get());
		if (!sessionAlive->load(std::memory_order_acquire) ||
		    g_generation.load(std::memory_order_acquire) != generation)
		{
			g_pLog->debug("Donor: discarded submission result from stale session\n");
			return;
		}
		if (!response.networkError && response.status == 200)
			g_pLog->info("Donor: submitted %zu code(s)\n", count);
		else
			g_pLog->info("Donor: submission failed HTTP=%ld network=%d\n",
			             response.status, response.networkError ? 1 : 0);
	}

	bool refreshWanted(const CConfig::DonateSettings& cfg, bool force = false)
	{
		const auto now = Clock::now();
		const std::string base = baseUrl(cfg);
		if (base != g_wantedBase)
		{
			g_wanted.clear();
			g_wantedFetchedAt = {};
			g_wantedBase = base;
			g_wantedAvailable = false;
			g_lastWantedAttemptMs = 0;
		}
		if (force) g_wantedFetchedAt = {};
		if (g_wantedFetchedAt != Clock::time_point{} &&
		    now - g_wantedFetchedAt < std::chrono::seconds(cfg.wantedRefreshSecs))
			return g_wantedAvailable;
		g_wantedFetchedAt = now;
		g_lastWantedAttemptMs.store(monotonicMs(), std::memory_order_release);
		const auto response = ManifestFetch::archiveRequest(
		    "GET", base + "/manifestwanted", {}, 32u * 1024u * 1024u,
		    30000, &g_running);
		if (response.networkError || response.status != 200)
		{
			// A transient failure should be retried on the next cycle, not
			// hidden for the full wanted-list refresh interval.
			g_wantedFetchedAt = now - std::chrono::seconds(cfg.wantedRefreshSecs)
			                  + std::chrono::seconds(std::min(cfg.intervalSecs, 30u));
			g_pLog->info("Donor: wanted list unavailable HTTP=%ld network=%d (%s)\n",
			             response.status, response.networkError ? 1 : 0,
			             response.diagnostic.c_str());
			return false;
		}

		std::vector<Wanted> fresh;
		std::string_view body(response.body);
		while (!body.empty())
		{
			const auto end = body.find('\n');
			std::string_view line = body.substr(0, end);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.remove_suffix(1);
			Wanted wanted;
			if (parseWantedLine(line, wanted)) fresh.push_back(wanted);
			if (end == body.npos) break;
			body.remove_prefix(end + 1);
		}
		static std::mt19937 random{std::random_device{}()};
		std::shuffle(fresh.begin(), fresh.end(), random);
		g_wanted = std::move(fresh);
		g_wantedAvailable = true;
		g_forceWantedRefresh.store(false, std::memory_order_release);
		g_pLog->info("Donor: wanted list has %zu entries\n", g_wanted.size());
		return true;
	}

	uint64_t requestCode(uint32_t appId, uint32_t depotId, uint64_t gid)
	{
		auto request = ManifestCode::requestCode(appId, depotId, gid);
		const auto deadline = Clock::now() + std::chrono::seconds(30);
		while (g_running && Clock::now() < deadline &&
		       request.result.wait_for(std::chrono::milliseconds(100)) !=
		           std::future_status::ready)
		{
		}
		if (!g_running || request.result.wait_for(std::chrono::seconds(0)) !=
		                      std::future_status::ready)
		{
			g_pLog->info("Donor: request stopped or timed out for depot=%u gid=%llu\n",
			             depotId, static_cast<unsigned long long>(gid));
			ManifestCode::discardRequest(request.jobId);
			return 0;
		}
		return request.result.get();
	}

	void flushCapturedCodes();

	void runCycle(const CConfig::DonateSettings& cfg)
	{
		std::unordered_set<uint32_t> owned;
		uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> sessionAlive;
		{
			std::lock_guard lock(g_ownedMutex);
			if (!g_licenseReady) return;
			owned = g_ownedDepots;
			generation = g_generation.load();
			sessionAlive = g_sessionAlive;
		}
		if (owned.empty() || !sessionAlive || !sessionAlive->load()) return;
		if (cfg.maxMintsPerSession &&
		    g_mintedThisSession.load() >= cfg.maxMintsPerSession) return;
		if (!refreshWanted(cfg)) return;
		if (g_wanted.empty()) return;

		const auto wantedEntries = g_wanted;
		std::string body;
		Clock::time_point batchStarted{};
		std::size_t pendingCount = 0;
		uint32_t attempted = 0, submitted = 0, refused = 0;
		std::size_t alreadyCached = 0, notOwned = 0;
		bool minted = false;
		auto flushBatch = [&]
		{
			if (!body.empty())
				submit(cfg, body, pendingCount, generation, sessionAlive);
			body.clear();
			pendingCount = 0;
			batchStarted = {};
		};
		for (const Wanted& wanted : wantedEntries)
		{
			// Passive codes were minted by Steam already and age independently of
			// this proactive sweep. Drain them between every potentially slow HEAD
			// or request-code operation so a long cycle cannot expire the queue.
			flushCapturedCodes();
			if (batchStarted != Clock::time_point{} &&
			    shouldFlushMintBatch(
				    pendingCount,
				    std::chrono::duration_cast<std::chrono::milliseconds>(
					    Clock::now() - batchStarted).count()))
				flushBatch();
			if (!g_running || g_generation.load() != generation ||
			    !sessionAlive->load(std::memory_order_acquire) ||
			    !g_config.donate.get().enabled ||
			    attempted >= cfg.maxMintsPerCycle ||
			    (cfg.maxMintsPerSession &&
			     g_mintedThisSession.load() >= cfg.maxMintsPerSession)) break;
			if (!owned.contains(wanted.depotId))
			{
				++notOwned;
				continue;
			}
			const auto cached = ManifestFetch::archiveRequest(
			    "HEAD", baseUrl(cfg) + "/m/" + std::to_string(wanted.depotId)
			    + "/" + std::to_string(wanted.gid), {}, 0, 5000, &g_running);
			if (!cached.networkError && cached.status == 200)
			{
				++alreadyCached;
				continue;
			}
			if (minted && cfg.minMintIntervalMs)
			{
				const auto deadline = Clock::now()
				    + std::chrono::milliseconds(cfg.minMintIntervalMs);
				while (Clock::now() < deadline && g_running &&
				       g_generation.load() == generation &&
				       sessionAlive->load(std::memory_order_acquire) &&
				       g_config.donate.get().enabled)
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (g_generation.load() != generation ||
			    !sessionAlive->load(std::memory_order_acquire) ||
			    !g_config.donate.get().enabled)
				break;
			minted = true;
			uint32_t appId = wanted.appId;
			if (!appId)
			{
				std::lock_guard lock(g_ownedMutex);
				if (const auto it = g_seenDepots.find(wanted.depotId);
				    it != g_seenDepots.end()) appId = it->second.appId;
			}
			g_pLog->debug("Donor: requesting app=%u depot=%u gid=%llu\n",
			             appId, wanted.depotId,
			             static_cast<unsigned long long>(wanted.gid));
			const uint64_t code = requestCode(appId, wanted.depotId, wanted.gid);
			++attempted;
			flushCapturedCodes();
			if (g_generation.load() != generation ||
			    !sessionAlive->load(std::memory_order_acquire)) break;
			++g_mintedThisSession;
			if (code)
			{
				if (pendingCount == 0) batchStarted = Clock::now();
				body += std::to_string(wanted.depotId) + ":"
				      + std::to_string(wanted.gid) + ":" + std::to_string(code) + "\n";
				++pendingCount;
				++submitted;
			}
			else
			{
				++refused;
			}
		}
		if (g_running && g_generation.load() == generation &&
		    sessionAlive->load(std::memory_order_acquire))
			flushBatch();
		if (attempted)
			g_pLog->info("Donor: cycle submitted=%u refused=%u archived=%zu "
			             "not_owned=%zu session=%u\n", submitted, refused,
			             alreadyCached, notOwned, g_mintedThisSession.load());
		else
			g_pLog->debug("Donor: cycle had no eligible entries (archived=%zu "
			              "not_owned=%zu)\n", alreadyCached, notOwned);
	}

	void processProbeFile()
	{
		static std::filesystem::file_time_type lastWrite{};
		const auto path = std::filesystem::path(g_config.getDir()) / "manifest_probe.txt";
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(path, ec);
		if (ec || stamp == lastWrite) return;
		lastWrite = stamp;
		if (std::filesystem::file_size(path, ec) > 65536 || ec) return;
		std::ifstream in(path);
		std::string line;
		for (unsigned int count = 0; count < 256 && std::getline(in, line); ++count)
		{
			const auto comment = line.find('#');
			if (comment != std::string::npos) line.erase(comment);
			const auto first = line.find_first_not_of(" \t\r");
			if (first == line.npos) continue;
			const auto last = line.find_last_not_of(" \t\r");
			const std::string_view value(line.data() + first, last - first + 1);
			uint32_t depot = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), depot);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
			    !depot) continue;
			Wanted seen;
			{
				std::lock_guard lock(g_ownedMutex);
				const auto it = g_seenDepots.find(depot);
				if (it == g_seenDepots.end()) continue;
				seen = it->second;
			}
			const uint64_t code = requestCode(seen.appId, depot, seen.gid);
			g_pLog->info("Manifest probe: depot=%u gid=%llu result=%s\n",
			             depot, static_cast<unsigned long long>(seen.gid),
			             code ? "success" : "failed");
		}
	}

	void flushCapturedCodes()
	{
		{
			std::lock_guard lock(g_captureMutex);
			if (g_capturedCodes.empty()) return;
		}
		const auto cfg = g_config.donate.get();
		if (!cfg.enabled)
		{
			std::lock_guard lock(g_captureMutex);
			g_capturedCodes.clear();
			return;
		}

		const bool forceRefresh = g_forceWantedRefresh.exchange(false);
		if (!refreshWanted(cfg, forceRefresh))
		{
			const auto cutoff = Clock::now() - std::chrono::minutes(4);
			std::lock_guard lock(g_captureMutex);
			std::erase_if(g_capturedCodes,
			              [cutoff](const CapturedCode& item)
			              {
				              return item.capturedAt < cutoff;
			              });
			return;
		}

		std::unordered_set<ManifestKey, ManifestKeyHash> wanted;
		wanted.reserve(g_wanted.size());
		for (const auto& item : g_wanted)
			wanted.insert({item.depotId, item.gid});

		std::unordered_set<uint32_t> owned;
		uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> sessionAlive;
		bool ownershipComplete = false;
		{
			std::lock_guard lock(g_ownedMutex);
			owned = g_ownedDepots;
			generation = g_generation.load();
			sessionAlive = g_sessionAlive;
			std::size_t resolved = 0;
			for (const uint32_t packageId : g_licenses)
				if (g_packages.contains(packageId)) ++resolved;
			ownershipComplete = g_licenseReady && resolved == g_licenses.size();
		}

		std::vector<CapturedCode> captured;
		{
			std::lock_guard lock(g_captureMutex);
			captured.swap(g_capturedCodes);
		}
		std::string body;
		std::vector<CapturedCode> retained;
		const auto cutoff = Clock::now() - std::chrono::minutes(4);
		std::size_t count = 0, stale = 0, expiredCount = 0;
		std::size_t notOwned = 0, notWanted = 0;
		for (const auto& item : captured)
		{
			const bool current = item.generation == generation;
			const bool licensed = owned.contains(item.depotId);
			const bool exactWanted = wanted.contains({item.depotId, item.gid});
			const bool expired = item.capturedAt < cutoff;
			if (!shouldSubmitCapturedCode(
			        cfg.enabled, current, licensed, exactWanted, !expired))
			{
				if (shouldRetainCapturedCode(
				        current, exactWanted, ownershipComplete, licensed,
				        expired))
				{
					retained.push_back(item);
					continue;
				}
				if (expired) ++expiredCount;
				else if (!current) ++stale;
				else if (!licensed) ++notOwned;
				else ++notWanted;
				continue;
			}
			body += std::to_string(item.depotId) + ":" + std::to_string(item.gid)
			      + ":" + std::to_string(item.code) + "\n";
			++count;
		}
		if (count && sessionAlive)
			submit(cfg, body, count, generation, sessionAlive);
		if (!retained.empty())
		{
			std::lock_guard lock(g_captureMutex);
			const std::size_t room = 256 - std::min<std::size_t>(
				256, g_capturedCodes.size());
			const std::size_t keep = std::min(room, retained.size());
			g_capturedCodes.insert(
				g_capturedCodes.end(), retained.begin(), retained.begin() + keep);
		}
		if (stale || expiredCount || notOwned || notWanted)
			g_pLog->debug(
				"Donor: passive filtered stale=%zu expired=%zu not_owned=%zu "
				"not_wanted=%zu\n",
				stale, expiredCount, notOwned, notWanted);
	}

	void worker()
	{
		g_pLog->info("Donor: started\n");
		auto nextCycle = Clock::now();
		while (g_running)
		{
			processProbeFile();
			flushCapturedCodes();
			if (Clock::now() >= nextCycle)
			{
				const auto cfg = g_config.donate.get();
				g_pLog->debug("Donor: cycle enabled=%d interval=%u session=%u\n",
				             cfg.enabled ? 1 : 0, cfg.intervalSecs,
				             g_mintedThisSession.load());
				if (cfg.enabled) runCycle(cfg);
				nextCycle = Clock::now() + std::chrono::seconds(cfg.intervalSecs);
			}
			std::unique_lock lock(g_sleepMutex);
			g_sleepCv.wait_for(lock, std::chrono::seconds(1), []
			{
				return !g_running.load(std::memory_order_acquire);
			});
		}
		g_pLog->info("Donor: stopped\n");
	}

	void startWorker()
	{
		std::lock_guard lock(g_workerMutex);
		if (g_stopping || g_running) return;
		if (g_worker.joinable()) g_worker.join();
		static std::once_flag cleanupOnce;
		static bool cleanupRegistered = false;
		std::call_once(cleanupOnce, []
		{
			cleanupRegistered = std::atexit(&ManifestDonor::stop) == 0;
		});
		if (!cleanupRegistered)
		{
			g_pLog->warn("Donor: process-exit cleanup registration failed\n");
			return;
		}
		g_running = true;
		try
		{
			g_worker = std::thread([]
			{
				ThreadStart::runGuarded(
					worker,
					[] { g_pLog->warn("Donor: worker failed\n"); },
					[] { g_running = false; });
			});
		}
		catch (...)
		{
			g_running = false;
			g_pLog->warn("Donor: failed to start worker\n");
		}
	}
}

void observePackage(const PackageInfo* package, bool available)
{
	if (!package || !package->PackageId) return;
	if (!available)
	{
		std::vector<std::pair<uint32_t, uint32_t>> licensedApps;
		StatsPolicy::Context statsContext{};
		{
			std::lock_guard lock(g_ownedMutex);
			g_packages.erase(package->PackageId);
			if (g_licenseReady)
			{
				(void)rebuildOwnedLocked(licensedApps);
				statsContext = StatsPolicy::context();
				publishLicensedApps(licensedApps, statsContext);
			}
		}
		return;
	}
	if (
	    package->AppIdVec.size > package->AppIdVec.memory.alloc ||
	    package->DepotIdVec.size >
	        package->DepotIdVec.memory.alloc ||
	    package->AppIdVec.size > 100000 ||
	    package->DepotIdVec.size > 100000 ||
	    (package->AppIdVec.size && !package->AppIdVec.memory.base) ||
	    (package->DepotIdVec.size && !package->DepotIdVec.memory.base))
		return;
	PackageRecord record;
	record.apps.reserve(package->AppIdVec.size);
	record.depots.reserve(package->DepotIdVec.size);
	for (uint32_t i = 0; i < package->AppIdVec.size; ++i)
	{
		const uint32_t app = package->AppIdVec.memory.base[i];
		if (app) record.apps.push_back(app);
	}
	for (uint32_t i = 0; i < package->DepotIdVec.size; ++i)
	{
		const uint32_t depot = package->DepotIdVec.memory.base[i];
		if (depot) record.depots.push_back(depot);
	}
	std::vector<std::pair<uint32_t, uint32_t>> licensedApps;
	StatsPolicy::Context statsContext{};
	std::size_t resolvedPackages = 0;
	std::size_t licenseCount = 0;
	std::size_t depotCount = 0;
	bool licensed = false;
	{
		std::lock_guard lock(g_ownedMutex);
		if (g_packages.size() >= 100000 &&
		    !g_packages.contains(package->PackageId)) return;
		g_packages[package->PackageId] = std::move(record);
		licensed = shouldObserveLicensedPackage(
			g_licenseReady, g_licenses.contains(package->PackageId));
		if (licensed)
		{
			statsContext = StatsPolicy::context();
			resolvedPackages = rebuildOwnedLocked(licensedApps);
			licenseCount = g_licenses.size();
			depotCount = g_ownedDepots.size();
			publishLicensedApps(licensedApps, statsContext);
		}
	}
	if (licensed && resolvedPackages == licenseCount)
		g_pLog->infoOnce(
			"Donor: all %zu licensed package(s) resolved (%zu depot(s))\n",
			licenseCount, depotCount);
}

void onLicenseList(const CMsgClientLicenseList* message)
{
	if (!message) return;
	std::unordered_set<uint32_t> licenses;
	if ((!message->has_eresult() || message->eresult() == 1) &&
	    message->licenses_size() <= 100000)
	{
		for (const auto& license : message->licenses())
			if (license.has_package_id() && license.package_id())
				licenses.insert(license.package_id());
	}
	std::vector<std::pair<uint32_t, uint32_t>> licensedApps;
	std::size_t resolvedPackages = 0;
	std::size_t licenseCount = 0;
	std::size_t depotCount = 0;
	StatsPolicy::Context statsContext{};
	{
		std::lock_guard lock(g_ownedMutex);
		const bool newSession = !g_licenseReady;
		g_licenses = std::move(licenses);
		g_licenseReady = !message->has_eresult() || message->eresult() == 1;
		resolvedPackages = rebuildOwnedLocked(licensedApps);
		++g_generation;
		g_sessionAlive->store(false, std::memory_order_release);
		g_sessionAlive = std::make_shared<std::atomic<bool>>(g_licenseReady);
		statsContext = StatsPolicy::context();
		publishLicensedApps(licensedApps, statsContext);
		if (newSession) g_mintedThisSession = 0;
		licenseCount = g_licenses.size();
		depotCount = g_ownedDepots.size();
	}
	g_pLog->info(
		"Donor: licenses=%zu resolved_packages=%zu unresolved_packages=%zu "
		"resolved_depots=%zu\n",
		licenseCount, resolvedPackages, licenseCount - resolvedPackages, depotCount);
	startWorker();
}

void onLoggedOff()
{
	{
		std::lock_guard lock(g_ownedMutex);
		g_licenseReady = false;
		g_licenses.clear();
		g_ownedDepots.clear();
		g_seenDepots.clear();
		++g_generation;
		g_sessionAlive->store(false, std::memory_order_release);
		g_mintedThisSession = 0;
	}
	std::lock_guard lock(g_captureMutex);
	g_capturedCodes.clear();
	g_forceWantedRefresh = false;
}

uint64_t sessionGeneration()
{
	return g_generation.load(std::memory_order_acquire);
}

void observeDepot(uint32_t appId, uint32_t depotId, uint64_t gid)
{
	if (!appId || !depotId || !gid) return;
	std::lock_guard lock(g_ownedMutex);
	if (g_seenDepots.size() >= 100000 && !g_seenDepots.contains(depotId)) return;
	g_seenDepots[depotId] = {appId, depotId, gid};
}

void submitCapturedCode(uint32_t depotId, uint64_t gid, uint64_t code,
                        uint64_t generation)
{
	const auto cfg = g_config.donate.get();
	if (!depotId || !gid || !code || !cfg.enabled ||
	    generation != g_generation.load(std::memory_order_acquire)) return;
	std::lock_guard lock(g_captureMutex);
	if (generation != g_generation.load(std::memory_order_acquire)) return;
	const auto cutoff = Clock::now() - std::chrono::minutes(4);
	std::erase_if(g_capturedCodes,
	              [cutoff](const CapturedCode& item)
	              {
		              return item.capturedAt < cutoff;
	              });
	if (g_capturedCodes.size() >= 256)
	{
		g_pLog->infoOnce("Donor: passive submission queue is full\n");
		return;
	}
	g_capturedCodes.push_back(
		{depotId, gid, code, generation, Clock::now()});
	const int64_t now = monotonicMs();
	if (shouldForceWantedRefresh(
		    now, g_lastWantedAttemptMs.load(std::memory_order_acquire),
		    static_cast<int64_t>(cfg.wantedRefreshSecs) * 1000))
		g_forceWantedRefresh = true;
}

void stop()
{
	std::unique_lock lock(g_workerMutex);
	g_stopping = true;
	g_running = false;
	{
		std::lock_guard ownedLock(g_ownedMutex);
		g_sessionAlive->store(false, std::memory_order_release);
	}
	g_sleepCv.notify_all();
	if (!g_worker.joinable()) return;
	if (g_worker.get_id() == std::this_thread::get_id())
		g_worker.detach();
	else
	{
		lock.unlock();
		g_worker.join();
	}
}
}
