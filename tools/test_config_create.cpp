// Config file creation (make test-config-create). CConfig::init() runs inside
// the loader's preinit callback, where an exception terminates Steam, so
// creating the config must report failure instead of throwing.
#include "../src/config.hpp"
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

	// createFile() with XDG_CONFIG_HOME set to `configHome`; `threw` reports
	// an exception instead of a result.
	bool createIn(const std::string& configHome, bool& threw)
	{
		setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
		threw = false;
		try
		{
			return g_config.createFile();
		}
		catch (...)
		{
			threw = true;
			return false;
		}
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
	const std::string root = "/tmp/slssteam-config-create." + std::to_string(getpid());
	std::filesystem::create_directories(root);
	setenv("HOME", root.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));
	t_suppressNotify = true; // the failure case must not run notify-send

	bool threw = false;
	const std::string missingParent = root + "/missing/config";
	const bool created = createIn(missingParent, threw);
	check(!threw, "a missing XDG_CONFIG_HOME does not throw");
	check(created && std::filesystem::exists(missingParent + "/SLSsteam/config.yaml"),
	      "a missing XDG_CONFIG_HOME is created with the config");

	const std::string fileParent = root + "/not-a-directory";
	std::ofstream(fileParent) << "x";
	const bool blocked = createIn(fileParent, threw);
	check(!threw, "a config directory that cannot be created does not throw");
	check(!blocked, "a config directory that cannot be created reports failure");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_config_create: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
