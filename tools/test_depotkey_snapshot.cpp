// The depot key index loads from a snapshot of the key folder and reads only
// the key files that changed since it was written (make test-depotkey-snapshot).
// Each load runs in a fresh child process, and inotify counts the key files the
// load opens.
#include "../src/config.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/log.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace
{
	int failures = 0;
	void check(bool condition, const std::string& message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message.c_str());
		if (!condition)
		{
			++failures;
		}
	}
	std::string g_cache;
	// A 32-byte key, as raw bytes and as the base64 the key files hold.
	std::string rawKey(char fill) { return std::string(32, fill); }
	std::string base64Key(char fill)
	{
		static const char* table =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const std::string raw = rawKey(fill);
		std::string out;
		for (std::size_t i = 0; i < raw.size(); i += 3)
		{
			const unsigned b0 = static_cast<unsigned char>(raw[i]);
			const unsigned b1 = i + 1 < raw.size() ? static_cast<unsigned char>(raw[i + 1]) : 0;
			const unsigned b2 = i + 2 < raw.size() ? static_cast<unsigned char>(raw[i + 2]) : 0;
			out += table[b0 >> 2];
			out += table[((b0 & 3) << 4) | (b1 >> 4)];
			out += i + 1 < raw.size() ? table[((b1 & 15) << 2) | (b2 >> 6)] : '=';
			out += i + 2 < raw.size() ? table[b2 & 63] : '=';
		}
		return out;
	}
	// Writes a key file the way an older build or another tool would.
	void writeKeyFile(uint32_t appId, uint32_t depotId, char fill)
	{
		const std::string path = g_cache + "/depotkey_" + std::to_string(depotId) + ".yaml";
		const std::string temp = path + ".tmp";
		std::ofstream(temp) << "appId: " << appId << "\ndepotId: " << depotId
		                    << "\nkey: " << base64Key(fill) << "\nmanaged: true\n";
		std::filesystem::rename(temp, path);
	}
	struct LoadResult
	{
		int keyFilesOpened = -1;
		bool keysMatch = false;
	};
	// Loads the index in a fresh child process: counts the key files the first
	// lookup opens, then checks every expected key (fill 0 = must be absent).
	LoadResult loadInChild(const std::vector<std::pair<uint32_t, char>>& expected)
	{
		int pipeFds[2];
		if (pipe(pipeFds) != 0) return {};
		const pid_t child = fork();
		if (child == 0)
		{
			close(pipeFds[0]);
			const int watch = inotify_init1(IN_NONBLOCK);
			inotify_add_watch(watch, g_cache.c_str(), IN_OPEN);
			(void)DepotKey::getCachedKey(expected.front().first);
			int opened = 0;
			alignas(inotify_event) char buffer[64 * 1024];
			for (;;)
			{
				const ssize_t got = read(watch, buffer, sizeof(buffer));
				if (got <= 0) break;
				for (ssize_t at = 0; at < got;)
				{
					const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
					const std::string name = event->len ? event->name : "";
					if (name.rfind("depotkey_", 0) == 0 && name.size() > 5
					    && name.compare(name.size() - 5, 5, ".yaml") == 0)
						++opened;
					at += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
				}
			}
			bool match = true;
			for (const auto& [depotId, fill] : expected)
			{
				const auto saved = DepotKey::getCachedKey(depotId);
				match = match && (fill == 0 ? saved.key.empty() : saved.key == rawKey(fill));
			}
			const LoadResult result {opened, match};
			(void)!write(pipeFds[1], &result, sizeof(result));
			std::_Exit(0);
		}
		close(pipeFds[1]);
		LoadResult result;
		if (read(pipeFds[0], &result, sizeof(result)) != sizeof(result)) result = {};
		close(pipeFds[0]);
		int status = 0;
		waitpid(child, &status, 0);
		return result;
	}
}
// The config translation unit publishes configured DLC ids to the apps
// feature, which this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	const std::string root = "/tmp/slssteam-depotkey-snapshot." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	g_cache = configHome + "/SLSsteam/cache";
	std::filesystem::create_directories(g_cache);
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));
	// The parent never loads the index, so every child starts without one.
	std::vector<std::pair<uint32_t, char>> keys;
	for (uint32_t i = 0; i < 40; ++i)
	{
		writeKeyFile(7, 1000 + i, static_cast<char>('A' + i % 20));
		keys.push_back({1000 + i, static_cast<char>('A' + i % 20)});
	}
	auto result = loadInChild(keys);
	check(result.keyFilesOpened == 40 && result.keysMatch,
	      "the first load reads all 40 key files (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	result = loadInChild(keys);
	check(result.keyFilesOpened == 0 && result.keysMatch,
	      "a later load reads no unchanged key file (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	writeKeyFile(7, 1005, 'z');
	keys[5].second = 'z';
	result = loadInChild(keys);
	check(result.keyFilesOpened == 1 && result.keysMatch,
	      "a rewritten key file is read again and its new key is used (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	std::filesystem::remove(g_cache + "/depotkey_1006.yaml");
	keys[6].second = 0;
	writeKeyFile(9, 2000, 'q');
	keys.push_back({2000, 'q'});
	result = loadInChild(keys);
	check(result.keyFilesOpened == 1 && result.keysMatch,
	      "a removed key file drops its key and a new one is read (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	// A damaged snapshot is ignored: the load reads every key file again.
	bool damaged = false;
	for (const auto& entry : std::filesystem::directory_iterator(g_cache))
	{
		const std::string name = entry.path().filename().string();
		if (name.rfind("depotkey_", 0) == 0 || name.find(".tmp") != std::string::npos) continue;
		std::ofstream(entry.path(), std::ios::trunc) << "not a snapshot\n";
		damaged = true;
	}
	result = loadInChild(keys);
	check(damaged && result.keyFilesOpened == 40 && result.keysMatch,
	      "a damaged snapshot falls back to reading every key file (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	// A key added while Steam runs (a hot-added script) reaches the running
	// index at once and the next process.
	const pid_t child = fork();
	if (child == 0)
	{
		const bool saved = DepotKey::saveKeyToCache(11, 3000, rawKey('h'), true);
		const auto depots = DepotKey::managedDepotsForApp(11);
		std::_Exit(saved && DepotKey::getCachedKey(3000).key == rawKey('h')
		           && std::find(depots.begin(), depots.end(), 3000u) != depots.end() ? 0 : 1);
	}
	int status = 0;
	waitpid(child, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "a key saved after the index loaded is served in that process");
	keys.push_back({3000, 'h'});
	result = loadInChild(keys);
	check(result.keyFilesOpened == 1 && result.keysMatch,
	      "the next process reads only that new key file (opened " +
	      std::to_string(result.keyFilesOpened) + ")");
	// Package 0 takes every keyed depot of the given apps from the index,
	// observed keys included, instead of reading the key files again.
	{
		std::ofstream(g_cache + "/depotkey_4000.yaml")
			<< "appId: 9\ndepotId: 4000\nkey: " << base64Key('o') << "\nmanaged: false\n";
		const pid_t lister = fork();
		if (lister == 0)
		{
			auto depots = DepotKey::depotsForApps({9, 11});
			std::sort(depots.begin(), depots.end());
			std::_Exit(depots == std::vector<uint32_t>{2000, 3000, 4000} ? 0 : 1);
		}
		int listed = 0;
		waitpid(lister, &listed, 0);
		check(WIFEXITED(listed) && WEXITSTATUS(listed) == 0,
		      "depotsForApps lists every keyed depot of the given apps, observed ones included");
	}
	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_depotkey_snapshot: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("all depot key snapshot checks passed\n");
	return 0;
}
