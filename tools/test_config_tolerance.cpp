// Regression test for config keys the file and the build disagree on (make
// test-config-tolerance). Users and lumen keep writing keys this build no
// longer reads; the real loader must ignore them without an error notice and
// still apply every other setting. A file that lacks a key (older, trimmed or
// unparsable) must get the shipped template's value for it.

#include "../src/config.hpp"
#include "../src/config_default.hpp"
#include "../src/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
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

	template<typename T>
	concept HasSafeMode = requires(T config) { config.safeMode; };

	template<typename T>
	concept HasWarnHashMissmatch = requires(T config) { config.warnHashMissmatch; };
}

// loadSettings publishes configured DLC ids to the apps feature, which this
// test does not link; the publications are recorded instead.
std::vector<std::vector<uint32_t>> g_dlcPublications;
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>& ids)
	{
		g_dlcPublications.push_back(ids);
	}
}

int main()
{
	const std::string root = "/tmp/slssteam-config-tolerance." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	std::filesystem::create_directories(configHome + "/SLSsteam");
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	// Keys removed here or upstream: SafeMode and WarnHashMissmatch (SafeMode
	// removal), PlayNotOwnedGames and AutoFilterList (upstream 84c3672,
	// d5a17c7), LogLevels (upstream's logger, not taken).
	const std::vector<std::string> removed = {
		"SafeMode", "WarnHashMissmatch", "PlayNotOwnedGames", "AutoFilterList", "LogLevels",
	};
	std::string text = std::string(defaultConfig) +
		"\nSafeMode: yes\nWarnHashMissmatch: yes\nPlayNotOwnedGames: yes\n"
		"AutoFilterList: yes\nLogLevels: 255\n";
	text.replace(text.find("FakeEmail: \"\""), std::string("FakeEmail: \"\"").size(),
	             "FakeEmail: \"kept@example.invalid\"");
	std::ofstream(configHome + "/SLSsteam/config.yaml") << text;

	check(g_config.loadSettings(), "a config with removed keys loads");
	check(g_config.fakeEmail.get() == "kept@example.invalid", "other settings still apply");
	const std::string errors = g_config.__loadErrors.get();
	for (const auto& key : removed)
	{
		check(errors.find(key) == std::string::npos, key + " produces no load error");
		check(std::string(defaultConfig).find("\n" + key + ":") == std::string::npos,
		      "the shipped config no longer carries " + key);
	}
	check(!HasSafeMode<CConfig> && !HasWarnHashMissmatch<CConfig>,
	      "SafeMode settings are gone from CConfig");

	const auto settings = []
	{
		const auto donate = g_config.donate.get();
		const auto n = [](auto value) { return std::to_string(value); };
		return std::vector<std::pair<std::string, std::string>>{
			{"DisableFamilyShareLock", n(g_config.disableFamilyLock.get())},
			{"DisableParentalRestrictions", n(g_config.disableParentalRestrictions.get())},
			{"UseWhitelist", n(g_config.useWhiteList.get())},
			{"SmartTickets", n(g_config.smartTickets.get())},
			{"MaxSchemaTries", n(g_config.maxSchemaTries.get())},
			{"Notifications", n(g_config.notifications.get())},
			{"NotifyInit", n(g_config.notifyInit.get())},
			{"API", n(g_config.api.get())},
			{"Plugins", n(g_config.plugins.get())},
			{"DisableCloud", n(g_config.disableCloud.get())},
			{"AutoUpdateApps", n(g_config.autoUpdateApps.get())},
			{"Donate.Enabled", n(donate.enabled)},
			{"Donate.Url", donate.url},
			{"Donate.IntervalSecs", n(donate.intervalSecs)},
			{"Donate.WantedRefreshSecs", n(donate.wantedRefreshSecs)},
			{"Donate.MaxMintsPerCycle", n(donate.maxMintsPerCycle)},
			{"Donate.MinMintIntervalMs", n(donate.minMintIntervalMs)},
			{"Donate.MaxMintsPerSession", n(donate.maxMintsPerSession)},
			{"InjectAllAdvertisedDlc", n(g_config.injectAllAdvertisedDlc.get())},
			{"Achievements", n(g_config.achievements.get())},
			{"AchievementOwnerId", n(g_config.achievementOwnerId.get())},
			{"PatternCache", n(g_config.patternCache.get())},
			{"AsyncProvision", n(g_config.asyncProvision.get())},
			{"MaxManagedApps", n(g_config.maxManagedApps.get())},
			{"DisableUpdates", n(g_config.disableUpdates.get())},
			{"FakeName", g_config.fakeName.get()},
			{"FakeEmail", g_config.fakeEmail.get()},
			{"FakeWalletBalance", n(g_config.fakeWalletBalance.get())},
			{"LogLevel", n(g_config.logLevel.get())},
			{"DumpClientInterfaces", n(g_config.dumpInterfaceMaps.get())},
			{"ExtendedLogging", n(g_config.extendedLogging.get())},
		};
	};
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc) << defaultConfig;
	check(g_config.loadSettings(), "the shipped config loads");
	const auto shipped = settings();
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc) << "{}\n";
	check(g_config.loadSettings(), "a config without settings loads");
	const auto missing = settings();
	for (std::size_t i = 0; i < shipped.size(); ++i)
	{
		if (shipped[i].first == "AchievementOwnerId")
			continue; // checked below
		check(missing[i].second == shipped[i].second,
		      "a missing " + shipped[i].first + " gets the template value " +
		      shipped[i].second + " (got " + missing[i].second + ")");
	}

	// New installs name no achievements owner (reviewers are looked up). Configs
	// from before the template carried the key keep the account they used.
	const auto ownerOf = [](const auto& values)
	{
		for (const auto& [key, value] : values)
			if (key == "AchievementOwnerId") return value;
		return std::string("<absent>");
	};
	check(ownerOf(shipped) == "0", "the template names no achievements owner");
	check(ownerOf(missing) == "76561198028121353",
	      "a config without AchievementOwnerId keeps the account it used");
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
		<< "AchievementOwnerId: 76561198000000001\n";
	check(g_config.loadSettings() && g_config.achievementOwnerId.get() == 76561198000000001ULL,
	      "an explicit AchievementOwnerId applies");

	// A reload replaces the configured DLC scope in one publication, so game
	// pipes never see it emptied while the rest of the config is read.
	std::ofstream(configHome + "/SLSsteam/luaappids.yaml", std::ios::trunc)
		<< "AdditionalApps:\n  - 42\n";
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
		<< "DlcData:\n  42:\n    4201: \"Test DLC\"\n";
	g_dlcPublications.clear();
	check(g_config.loadSettings() && g_config.loadSettings(), "a config with DlcData reloads");
	check(g_dlcPublications == std::vector<std::vector<uint32_t>>{{4201}, {4201}},
	      "each reload publishes the configured DLC once, with no empty scope first");
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
		<< "AchievementOwnerId: 0\n";
	g_dlcPublications.clear();
	check(g_config.loadSettings() &&
	          g_dlcPublications == std::vector<std::vector<uint32_t>>{{}} &&
	          g_config.dlcData.get().empty(),
	      "removing DlcData revokes the configured DLC");

	// Scripts over MaxManagedApps stay inactive; the user is told once.
	const std::string steam = root + "/.local/share/Steam";
	std::filesystem::create_directories(steam + "/config/stplug-in");
	std::ofstream(steam + "/steam.sh") << "#!/bin/sh\n";
	for (const char* script : {"10.lua", "11.lua", "12.lua"})
		std::ofstream(steam + "/config/stplug-in/" + script) << "addappid(1)\n";
	std::ofstream(configHome + "/SLSsteam/luaappids.yaml", std::ios::trunc) << "";
	std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
		<< "MaxManagedApps: 1\nNotifications: no\n";
	check(g_config.loadSettings() && g_config.loadSettings(), "a config over the app limit loads");
	std::size_t notices = 0;
	{
		std::ifstream logFile(root + "/test.log");
		for (std::string line; std::getline(logFile, line);)
			if (line.find("notifyUser:") != std::string::npos && line.find("MaxManagedApps") != std::string::npos)
				++notices;
	}
	check(notices == 1, "scripts over MaxManagedApps are reported to the user once");

	// Every game add reloads the whole config: lists are logged as a count,
	// not one line per entry, and an unchanged error block is not repeated.
	const auto logCount = [&](const std::string& needle)
	{
		std::size_t count = 0;
		std::ifstream logFile(root + "/test.log");
		for (std::string line; std::getline(logFile, line);)
			if (line.find(needle) != std::string::npos)
				++count;
		return count;
	};
	{
		std::string large = "AdditionalApps:\n";
		for (int id = 100000; id < 100500; ++id)
			large += "  - " + std::to_string(id) + "\n";
		large += "AppTokens:\n";
		for (int id = 100000; id < 100500; ++id)
			large += "  " + std::to_string(id) + ": 1234\n";
		std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc) << large;
		const std::size_t before = logCount("] Added ");
		check(g_config.loadSettings() && g_config.loadSettings(), "a config with large lists reloads");
		check(logCount("] Added ") == before,
		      "reloads log no line per list entry (got " +
		      std::to_string(logCount("] Added ") - before) + ")");
	}
	{
		std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
			<< "AchievementOwnerId: 0\nNotifications: no\n";
		const std::size_t before = logCount("Config loading errors");
		check(g_config.loadSettings() && g_config.loadSettings(), "a config missing keys reloads");
		check(logCount("Config loading errors") - before == 1,
		      "an unchanged missing-key report is written once");
	}

	// A luaappids.yaml that does not parse is not an empty list: the apps it
	// listed stay managed until it parses again.
	{
		std::ofstream(configHome + "/SLSsteam/config.yaml", std::ios::trunc)
			<< "AchievementOwnerId: 0\nNotifications: no\n";
		const auto writeLuaAppIds = [&](const char* text)
		{
			std::ofstream(configHome + "/SLSsteam/luaappids.yaml", std::ios::trunc) << text;
		};
		writeLuaAppIds("AdditionalApps:\n  - 4242\n");
		check(g_config.loadSettings() && g_config.managedAppIds.contains(4242),
		      "luaappids.yaml apps are managed");
		writeLuaAppIds("AdditionalApps: [4242\n");
		check(g_config.loadSettings() && g_config.managedAppIds.contains(4242),
		      "a luaappids.yaml that does not parse keeps its apps managed");
		writeLuaAppIds("just text\n");
		check(g_config.loadSettings() && g_config.managedAppIds.contains(4242),
		      "a luaappids.yaml that is not a map keeps its apps managed");
		writeLuaAppIds("AdditionalApps: []\n");
		check(g_config.loadSettings() && !g_config.managedAppIds.contains(4242),
		      "an emptied luaappids.yaml removes its apps");
	}

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_config_tolerance: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
