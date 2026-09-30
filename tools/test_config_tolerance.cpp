// Regression test for removed config keys (make test-config-tolerance). Users
// and lumen keep writing keys this build no longer reads; the real loader must
// ignore them without an error notice and still apply every other setting.

#include "../src/config.hpp"
#include "../src/config_default.hpp"
#include "../src/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_config_tolerance: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
