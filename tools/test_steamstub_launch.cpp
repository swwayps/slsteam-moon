// Regression test for the SteamStub launch path (make test-steamstub-launch).
// It runs inside Steam's LaunchApp hook, so a filesystem error while looking up
// the install folder must be skipped instead of thrown into Steam.
#include "../src/config.hpp"
#include "../src/feats/steamstub.hpp"
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
}
// The config translation unit publishes configured DLC ids to the apps
// feature, which this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
int main()
{
	const std::string root = "/tmp/slssteam-steamstub-launch." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	const std::string steam = root + "/.local/share/Steam";
	std::filesystem::create_directories(configHome + "/SLSsteam");
	std::filesystem::create_directories(steam + "/steamapps");
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	// A library folder that cannot be resolved (a symlink loop fails with
	// ELOOP, like an unreachable mount) sits before the real library.
	std::filesystem::create_directory_symlink(root + "/loop", root + "/loop");
	std::ofstream(steam + "/steamapps/libraryfolders.vdf")
		<< "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << root
		<< "/loop\"\n\t}\n}\n";
	g_config.addedAppIds.set({123});

	bool threw = false;
	try { SteamStub::onLaunchApp(123); }
	catch (...) { threw = true; }
	check(!threw, "an unresolvable library folder does not throw out of the launch hook");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_steamstub_launch: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
