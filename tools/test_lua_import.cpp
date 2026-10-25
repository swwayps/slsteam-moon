// Regression test for the Lua script key importer (make test-lua-import). A
// depot id that does not fit 32 bits is skipped instead of thrown out of
// setup() or the config watcher thread, and the script's other lines still
// import.
#include "../src/config.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/log.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <iterator>
#include <string>
#include <sys/resource.h>
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

	check(!depotKeyThrew, "an oversized depot id does not throw out of the key importer");
	check(DepotKey::getCachedKey(124).key.size() == 32,
	      "the key line after the oversized one still imports");

	// A script folder that cannot be iterated (here: a file in its place, which
	// fails like an unreadable folder) is skipped, not thrown into setup().
	std::filesystem::remove_all(steam + "/config/stplug-in");
	std::ofstream(steam + "/config/stplug-in") << "not a folder\n";
	bool unreadableThrew = false;
	try { DepotKey::reloadLuaScripts(); }
	catch (...) { unreadableThrew = true; }
	check(!unreadableThrew, "an unreadable script folder does not throw out of the importer");

	// A key write that fails (here: the file size limit) is reported and
	// leaves the previous key file as it was.
	const auto readAll = [](const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};
	check(DepotKey::saveKeyToCache(7, 701, std::string(32, 'A'), true), "a key is saved");
	const std::string keyFile = DepotKey::getKeyPath(701);
	const std::string saved = readAll(keyFile);
	signal(SIGXFSZ, SIG_IGN);
	rlimit previous{};
	getrlimit(RLIMIT_FSIZE, &previous);
	rlimit tiny = previous;
	tiny.rlim_cur = 16;
	setrlimit(RLIMIT_FSIZE, &tiny);
	const bool savedOverLimit = DepotKey::saveKeyToCache(7, 701, std::string(32, 'B'), true);
	setrlimit(RLIMIT_FSIZE, &previous);
	check(!savedOverLimit, "a failed key write is reported");
	check(!saved.empty() && readAll(keyFile) == saved,
	      "a failed key write leaves the saved key file intact");

	// Manifests a script folder ships are copied into depotcache only whole.
	const auto writeManifest = [](const std::string& path, bool withEnd)
	{
		std::ofstream out(path, std::ios::binary);
		const uint32_t magic = 0x71F617D0u, end = 0x32C415ABu;
		out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
		out << "manifest-payload";
		if (withEnd) out.write(reinterpret_cast<const char*>(&end), sizeof(end));
	};
	std::filesystem::create_directories(steam + "/config/depotcache");
	writeManifest(steam + "/config/depotcache/1_2.manifest", true);
	writeManifest(steam + "/config/depotcache/3_4.manifest", false);
	DepotKey::provisionManifests();
	check(readAll(steam + "/depotcache/1_2.manifest") ==
	          readAll(steam + "/config/depotcache/1_2.manifest"),
	      "a complete shipped manifest is copied into depotcache");
	check(!std::filesystem::exists(steam + "/depotcache/3_4.manifest"),
	      "a cut-off shipped manifest is not copied into depotcache");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_lua_import: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
