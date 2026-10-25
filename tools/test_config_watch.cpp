// The config watcher reloads for config.yaml and luaappids.yaml, including a
// luaappids.yaml first created after boot, and ignores every other file in the
// config folder (make test-config-watch).
#include "../src/config.hpp"
#include "../src/config_default.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/feats/packagepatch.hpp"
#include "../src/feats/ticket.hpp"
#include "../src/log.hpp"
#include "../src/sdk/IClientCompat.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

// Collaborators of the reload callback that this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
namespace AppInfoProvision
{
	bool forgetApp(uint32_t) { return true; }
	bool forgetManagedSourceApp(uint32_t) { return true; }
}
namespace Ticket
{
	std::unordered_map<AppId_t, SavedTicket> ticketMap;
	std::unordered_map<AppId_t, SavedTicket> encryptedTicketMap;
}
namespace
{
	std::mutex g_mutex;
	std::vector<std::unordered_set<uint32_t>> g_published;
}
namespace HotReload
{
	void publish(const std::unordered_set<std::uint32_t>& managed, bool) noexcept
	{
		const std::lock_guard lock(g_mutex);
		g_published.push_back(managed);
	}
	bool publishPreparedBase(std::uint32_t, std::uint64_t) noexcept { return true; }
}
// Steam-side work the watcher's idle tick can reach; nothing is queued here.
namespace PackagePatch
{
	void synchronizePackage0(const PackageSnapshot&) {}
}
IClientCompat* g_pClientCompat = nullptr;
const char* IClientCompat::getCompatToolName(const AppId_t) { return ""; }
void IClientCompat::specifyCompatTool(const AppId_t, const char*, const char*, int32_t) {}

namespace
{
	int failures = 0;
	void check(bool condition, const std::string& message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message.c_str());
		if (!condition) ++failures;
	}

	std::size_t publishCount()
	{
		const std::lock_guard lock(g_mutex);
		return g_published.size();
	}

	// Wait until more than `seen` reloads happened, up to `limit`.
	bool reloadedAfter(std::size_t seen, std::chrono::milliseconds limit)
	{
		const auto end = std::chrono::steady_clock::now() + limit;
		while (std::chrono::steady_clock::now() < end)
		{
			if (publishCount() > seen) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return publishCount() > seen;
	}

	// Write through a temporary file and rename, as editors and the plugin do.
	void replaceFile(const std::string& path, const std::string& text)
	{
		const std::string temp = path + ".part";
		std::ofstream(temp) << text;
		std::filesystem::rename(temp, path);
	}
}

int main()
{
	const std::string root = "/tmp/slssteam-config-watch." + std::to_string(getpid());
	const std::string dir = root + "/config/SLSsteam";
	std::filesystem::create_directories(dir);
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	std::string text = defaultConfig;
	text.replace(text.find("Notifications: yes"), std::string("Notifications: yes").size(),
	             "Notifications: no");
	std::ofstream(dir + "/config.yaml") << text;
	check(g_config.init(), "config loads without luaappids.yaml");

	std::size_t seen = publishCount();
	replaceFile(dir + "/library-added-times.txt", "unrelated\n");
	check(!reloadedAfter(seen, std::chrono::milliseconds(800)),
	      "an unrelated file in the config folder does not reload");

	seen = publishCount();
	replaceFile(dir + "/luaappids.yaml", "AdditionalApps:\n  - 42\n");
	bool sawApp = reloadedAfter(seen, std::chrono::seconds(3));
	{
		const std::lock_guard lock(g_mutex);
		sawApp = sawApp && g_published.back().contains(42);
	}
	check(sawApp, "a luaappids.yaml created after boot is published");

	for (int round = 1; round <= 2; ++round)
	{
		seen = publishCount();
		replaceFile(dir + "/config.yaml", text);
		check(reloadedAfter(seen, std::chrono::seconds(3)),
		      "config.yaml replaced by rename reloads (round " + std::to_string(round) + ")");
	}

	std::filesystem::remove_all(root);
	std::printf("%s\n", failures == 0 ? "config watch: all passed" : "config watch: FAILED");
	return failures == 0 ? 0 : 1;
}
