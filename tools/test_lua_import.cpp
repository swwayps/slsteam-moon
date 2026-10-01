// Regression test for the Lua script importers (make test-lua-import). A depot
// id that does not fit 32 bits is skipped instead of thrown out of setup() or
// the config watcher thread, and the script's other lines still import.
#include "../src/config.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/feats/manifestid.hpp"
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
	const std::string root = "/tmp/slssteam-lua-import." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	const std::string steam = root + "/.local/share/Steam";
	std::filesystem::create_directories(configHome + "/SLSsteam/cache");
	std::filesystem::create_directories(steam + "/config/stplug-in");
	std::ofstream(steam + "/steam.sh") << "#!/bin/sh\n";
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	const std::string key(64, 'a');
	std::ofstream(steam + "/config/stplug-in/123.lua")
		<< "addappid(123)\n"
		<< "addappid(99999999999, 1, \"" << key << "\")\n"
		<< "addappid(124, 1, \"" << key << "\")\n"
		<< "setManifestid(99999999999, \"1\")\n"
		<< "setManifestid(124, \"456\")\n";
	g_config.managedAppIds.set({123});

	bool depotKeyThrew = false;
	try { DepotKey::reloadLuaScripts(); }
	catch (...) { depotKeyThrew = true; }
	bool manifestIdThrew = false;
	try { ManifestId::importLuaScripts(); }
	catch (...) { manifestIdThrew = true; }

	check(!depotKeyThrew, "an oversized depot id does not throw out of the key importer");
	check(!manifestIdThrew, "an oversized depot id does not throw out of the pin importer");
	check(DepotKey::getCachedKey(124).key.size() == 32,
	      "the key line after the oversized one still imports");
	check(ManifestId::getPinnedGid(124) == "456",
	      "the pin line after the oversized one still imports");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_lua_import: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
