#include "update.hpp"

#include "config.hpp"
#include "curl.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "utils.hpp"
#include "version.hpp"

#include "update_cache.hpp"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

std::map<uint64_t, std::unordered_set<std::string>> Updater::clientHashMap = std::map<uint64_t, std::unordered_set<std::string>>();

namespace
{
	// Guards clientHashMap so the background refresh thread and the
	// load()-time verifySafeModeHash() reader never race.
	std::mutex g_hashMapMtx;

	// Ensures we only ever spawn one background refresh worker.
	std::atomic<bool> g_refreshStarted{false};

	constexpr const char* kUpdatesUrl =
	    "https://raw.githubusercontent.com/AceSLS/SLSsteam/refs/heads/main/res/updates.yaml";

	// Freshness window for the on-disk updates.yaml cache.  The safe-mode
	// hash list changes only when Steam ships a new steamclient.so, so a
	// day is plenty; this keeps the default user off the network on every
	// relaunch.  Override via SLSSTEAM_UPDATES_TTL (seconds; 0 = always
	// refresh).
	long long updatesTtlSecs()
	{
		if (const char* ov = std::getenv("SLSSTEAM_UPDATES_TTL"); ov && *ov)
		{
			try { return std::stoll(ov); } catch (...) {}
		}
		return 24 * 3600; // 1 day
	}

	// Parse a updates.yaml document into the version->hashes map.  Pure
	// w.r.t. the network; returns false on malformed input.
	bool parseHashMap(const std::string& data,
	                  std::map<uint64_t, std::unordered_set<std::string>>& out)
	{
		try
		{
			YAML::Node node = YAML::Load(data);
			std::map<uint64_t, std::unordered_set<std::string>> parsed;
			for (const auto& sub : node["SafeModeHashes"])
			{
				uint64_t version = sub.first.as<uint64_t>();
				parsed[version] = std::unordered_set<std::string>();
				for (const auto& hash : sub.second)
				{
					parsed[version].emplace(hash.as<std::string>());
				}
			}
			out = std::move(parsed);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	// epoch-seconds mtime of the cache file, or -1 if absent/empty.
	long long cacheMtimeSecs()
	{
		struct stat st{};
		const auto path = Updater::getCacheFilePath();
		if (stat(path.c_str(), &st) != 0) return -1;
		if (st.st_size <= 0) return -1;
		return static_cast<long long>(st.st_mtime);
	}

	// Fetch updates.yaml from GitHub, parse it, swap it into the shared
	// map and persist the cache.  Returns true on success.  Safe to call
	// from any thread.
	bool fetchParseAndStore()
	{
		std::string data;
		const int res = Curl::getString(kUpdatesUrl, data);
		if (res != 0 || data.empty())
		{
			g_pLog->debug("Updater: network fetch failed (curl=%d)\n", res);
			return false;
		}

		std::map<uint64_t, std::unordered_set<std::string>> parsed;
		if (!parseHashMap(data, parsed))
		{
			g_pLog->info("Failed to parse updates!\n");
			return false;
		}

		{
			std::lock_guard<std::mutex> lk(g_hashMapMtx);
			Updater::clientHashMap = std::move(parsed);
		}
		Updater::saveToCache(data);
		return true;
	}
}

bool Updater::init()
{
	// When the safe-mode hash data actually gates behaviour this session
	// (SafeMode aborts, WarnHashMissmatch warns), it must be authoritative
	// before load()'s verifySafeModeHash() runs — so fetch synchronously,
	// exactly as before.  Falls back to the on-disk cache on a failed GET.
	if (cache::mustFetchSynchronously(g_config.safeMode.get(),
	                                  g_config.warnHashMissmatch.get()))
	{
		if (fetchParseAndStore())
		{
			return true;
		}
		const std::string cached = loadFromCache();
		if (cached.empty()) return false;
		g_pLog->info("Using cached updates.yaml\n");
		std::map<uint64_t, std::unordered_set<std::string>> parsed;
		if (!parseHashMap(cached, parsed)) return false;
		std::lock_guard<std::mutex> lk(g_hashMapMtx);
		clientHashMap = std::move(parsed);
		return true;
	}

	// Default path: do NOT touch the network on the preinit critical path.
	// Serve whatever cache we have synchronously (so verifySafeModeHash()
	// still has data), and let a background worker refresh it later off
	// the boot path (see refreshInBackgroundIfStale, called from a real
	// Steam worker thread in pics.cpp).
	const std::string cached = loadFromCache();
	if (!cached.empty())
	{
		std::map<uint64_t, std::unordered_set<std::string>> parsed;
		if (parseHashMap(cached, parsed))
		{
			std::lock_guard<std::mutex> lk(g_hashMapMtx);
			clientHashMap = std::move(parsed);
		}
	}
	return true;
}

void Updater::refreshInBackgroundIfStale()
{
	// Idempotent: only one refresh per process.  MUST be called from a
	// real Steam worker thread (e.g. the PICS recv path), never from the
	// LD_AUDIT load()/setup() path — spawning a thread there crashes Steam.
	bool expected = false;
	if (!g_refreshStarted.compare_exchange_strong(expected, true))
	{
		return;
	}

	const long long ttl = updatesTtlSecs();
	const long long mtime = cacheMtimeSecs();
	const long long now = static_cast<long long>(std::time(nullptr));
	if (cache::isCacheFresh(mtime >= 0, mtime, now, ttl))
	{
		g_pLog->debug("Updater: cache fresh (age<%llds), skipping refresh\n", ttl);
		return;
	}

	std::thread([] {
		if (fetchParseAndStore())
		{
			g_pLog->debug("Updater: background refresh of updates.yaml done\n");
		}
	}).detach();
}

std::string Updater::getCacheFilePath()
{
	auto path = g_config.getDir().append("/.updates.yaml");
	return path;
}

void Updater::saveToCache(std::string yaml)
{
	auto path = Updater::getCacheFilePath();

	std::ofstream stream = std::ofstream(path.c_str());
	stream << yaml;
	stream.close();

	g_pLog->debug("Cached res/updates.yaml!\n");
}

std::string Updater::loadFromCache()
{
	auto path = Updater::getCacheFilePath();
	if (!std::filesystem::exists(path))
	{
		return std::string();
	}

	g_pLog->debug("Loading updates.ymal from disk!\n");

	std::ifstream fstream = std::ifstream(path.c_str());
	std::stringstream buf;
	buf << fstream.rdbuf();

	fstream.close();
	return buf.str();
}

bool Updater::verifySafeModeHash()
{
	auto path = std::filesystem::path(g_modSteamClient.path);

	// Nothing consumes the digest with the default config (SafeMode: no,
	// WarnHashMissmatch: no), and computing it streams the whole 49 MB
	// steamclient.so through SHA-256 *inside* the client's dlopen — 0.22 s of
	// blocking boot time per launch, measured. Report "verified" so load()
	// takes neither the abort nor the warn branch (both are no-ops when the
	// flags are off anyway).
	if (!cache::mustVerifyClientHash(g_config.safeMode.get(),
	                                 g_config.warnHashMissmatch.get(),
	                                 g_config.extendedLogging.get()))
	{
		g_pLog->debug("Skipping steamclient.so hash (no hash check enabled)\n");
		return true;
	}

	try
	{
		std::string sha256 = Utils::getFileSHA256(path.c_str());
		g_pLog->info("steamclient.so hash is %s\n", sha256.c_str());

		std::lock_guard<std::mutex> lk(g_hashMapMtx);
		if (!clientHashMap.contains(VERSION))
		{
			return false;
		}

		const auto& safeHashes = clientHashMap[VERSION];
		if (safeHashes.contains(sha256))
		{
			return true;
		}

		return false;
	}
	catch(std::runtime_error& err)
	{
		g_pLog->debug("Unable to read steamclient.so hash!\n");
		return false;
	}

	return true;
}
