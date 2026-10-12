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
// test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
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
		check(missing[i].second == shipped[i].second,
		      "a missing " + shipped[i].first + " gets the template value " +
		      shipped[i].second + " (got " + missing[i].second + ")");
	}

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_config_tolerance: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
