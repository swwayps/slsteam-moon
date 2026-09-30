// Regression test for the Lua plugin seams in CConfig (make test-plugin-config).
// Plugins stay off unless config.yaml says `Plugins: yes`. A configLoading hook
// sees the tree just parsed and its changes reach the settings; configLoaded
// runs once, after them. A plugin's setAdditionalApps keeps the managed apps.

#include "../src/config.hpp"
#include "../src/config_default.hpp"
#include "../src/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
		if (!condition)
		{
			++failures;
		}
	}

	std::vector<std::string> events;

	void onLoading(YAML::Node& node)
	{
		events.push_back("loading:" + node["FakeName"].as<std::string>(""));
		node["FakeName"] = "from-plugin";
	}

	void onLoaded()
	{
		events.push_back("loaded:" + g_config.fakeName.get());
	}

	void writeConfig(const std::string& path, const char* text)
	{
		std::ofstream(path, std::ios::trunc) << text;
	}
}

// loadSettings publishes configured DLC ids to the apps feature, which this
// test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}

int main()
{
	const std::string root = "/tmp/slssteam-plugin-config." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	const std::string configPath = configHome + "/SLSsteam/config.yaml";
	std::filesystem::create_directories(configHome + "/SLSsteam");
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	check(std::string(defaultConfig).find("\nPlugins: no\n") != std::string::npos,
	      "the shipped config keeps plugins off");

	writeConfig(configPath, "FakeName: from-file\n");
	g_config.loadSettings();
	check(!g_config.plugins.get(), "plugins are off without the key");
	check(g_config.fakeName.get() == "from-file", "settings load without plugin hooks");
	check(g_config.readTree()["FakeName"].as<std::string>("") == "from-file",
	      "readTree returns the file without applying it");

	g_config.pluginConfigLoading = &onLoading;
	g_config.pluginConfigLoaded = &onLoaded;
	writeConfig(configPath, "FakeName: from-file\nPlugins: yes\n");
	g_config.loadSettings();
	check(g_config.plugins.get(), "Plugins: yes enables plugins");
	check(events == std::vector<std::string>{"loading:from-file", "loaded:from-plugin"},
	      "configLoading sees the parsed file and configLoaded runs after the settings");
	check(g_config.fakeName.get() == "from-plugin", "a configLoading change reaches the settings");

	g_config.managedAppIds.set({10});
	g_config.setAdditionalApps({20});
	check(g_config.addedAppIds.get() == std::unordered_set<uint32_t>{10, 20},
	      "setAdditionalApps keeps the managed apps active");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_plugin_config: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
