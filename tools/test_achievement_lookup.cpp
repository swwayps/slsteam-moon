// Background reviewer lookup: one worker, every queued app answered once,
// failed downloads answered empty so the owner directory stops waiting.
#include "../src/curl.hpp"
#include "../src/feats/achievements.hpp"
#include "../src/log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
std::mutex g_mutex;
std::condition_variable g_changed;
std::map<uint32_t, std::vector<std::vector<uint64_t>>> g_offers;
std::vector<std::string> g_urls;
int g_saves = 0;
int g_failures = 0;

void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++g_failures;
}

bool waitForOffers(std::size_t total)
{
	std::unique_lock lock(g_mutex);
	return g_changed.wait_for(lock, std::chrono::seconds(5), [total] {
		std::size_t seen = 0;
		for (const auto& [app, offers] : g_offers) seen += offers.size();
		return seen >= total;
	});
}
}

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

int Curl::downloadString(const char* url, std::string& out, const int)
{
	{
		std::lock_guard lock(g_mutex);
		g_urls.emplace_back(url);
	}
	const std::string link(url);
	if (link.find("?appid=1&") == std::string::npos) return 1;
	out = R"({"reviews":[{"author":{"steamid":"76561198000000031"}},)"
	      R"({"author":{"steamid":"76561198000000032"}}]})";
	return 0;
}

void Achievements::saveOwners()
{
	{
		std::lock_guard lock(g_mutex);
		++g_saves;
	}
	g_changed.notify_all();
}

void Achievements::offerOwners(uint32_t app, const std::vector<uint64_t>& owners)
{
	{
		std::lock_guard lock(g_mutex);
		g_offers[app].push_back(owners);
	}
	g_changed.notify_all();
}

int main()
{
	Achievements::requestOwnerLookup(1, 5);
	Achievements::requestOwnerLookup(2, 5);
	check(waitForOffers(2), "every queued lookup is answered");
	{
		std::lock_guard lock(g_mutex);
		check(g_offers[1].size() == 1 &&
		      g_offers[1][0] == std::vector<uint64_t>({76561198000000031ULL, 76561198000000032ULL}),
		      "a fetched page yields its reviewers");
		check(g_offers[2].size() == 1 && g_offers[2][0].empty(),
		      "a failed download is answered empty");
		check(!g_urls.empty() && g_urls[0] == Achievements::reviewUrl(1, 5),
		      "the worker asks for the requested count");
	}
	Achievements::requestOwnerLookup(1, 1);
	check(waitForOffers(3), "a finished worker starts again for a later request");
	{
		std::lock_guard lock(g_mutex);
		check(g_offers[1].size() == 2 &&
		      g_offers[1][1] == std::vector<uint64_t>({76561198000000031ULL}),
		      "the later request keeps its own count");
	}
	Achievements::requestOwnerSave();
	{
		std::unique_lock lock(g_mutex);
		check(g_changed.wait_for(lock, std::chrono::seconds(5), [] { return g_saves >= 1; }),
		      "a save request writes the learned owners off Steam's threads");
	}
	std::printf("%d failure(s)\n", g_failures);
	return g_failures ? 1 : 0;
}
