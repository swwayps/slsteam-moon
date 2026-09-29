#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>


namespace Updater
{
	extern std::map<uint64_t, std::unordered_set<std::string>> clientHashMap;

	std::string getCacheFilePath();
	std::string loadFromCache();
	void saveToCache(const std::string yaml);

	bool init();
	bool verifySafeModeHash();

	// Refresh the on-disk updates.yaml cache from the network in the
	// background, but only if it is older than the TTL.  Idempotent.
	// MUST be called from a real Steam worker thread (the PICS recv path),
	// never from the LD_AUDIT load()/setup() path.
	void refreshInBackgroundIfStale();
}
