// pruneUnsupportedDepots keeps a DLC advertised when its only keyed depot is
// empty: the empty manifest is dropped, the entitlement is not.
// Build: make test-depot-prune (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/log.hpp"
#include "yaml-cpp/yaml.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
namespace AppInfoProvision
{
	void pruneUnsupportedDepots(YAML::Node& body, uint32_t appId,
	                            const CachePublicationToken& publication);
}
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_depot_prune_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((std::string(home) + "/test.log").c_str()));

	const std::string key(32, 'k');
	DepotKey::saveKeyToCache(42, 4200, key, true);
	DepotKey::saveKeyToCache(42, 4300, key, true);

	YAML::Node body = YAML::Load(
		"common: {oslist: linux}\n"
		"extended: {listofdlc: '43,44'}\n"
		"depots:\n"
		"  '4200': {config: {oslist: linux}, manifests: {public: {gid: '1', size: '100'}}}\n"
		"  '4300': {dlcappid: '43', manifests: {public: {gid: '2', size: '0'}}}\n"
		"  '4400': {dlcappid: '44', manifests: {public: {gid: '3', size: '50'}}}\n");
	AppInfoProvision::pruneUnsupportedDepots(body, 42, {});

	CHECK(!body["depots"]["4300"], "the empty DLC depot is dropped");
	CHECK(!body["depots"]["4400"], "an unkeyed DLC depot is dropped");
	CHECK(body["depots"]["4200"].IsDefined(), "the keyed base depot is kept");
	const std::string list = body["extended"]["listofdlc"].as<std::string>("");
	CHECK(list == "43",
	      ("the DLC with a keyed empty depot stays listed, the unkeyed one goes (got \"" +
	       list + "\")").c_str());

	// Shared untagged content and the Windows depot are keyed, the Linux depot
	// is not: only Proton can run the game.
	DepotKey::saveKeyToCache(50, 5001, key, true);
	DepotKey::saveKeyToCache(50, 5002, key, true);
	g_config.managedAppIds.set({50});
	YAML::Node shared = YAML::Load(
		"common: {oslist: 'windows,linux'}\n"
		"depots:\n"
		"  '5001': {manifests: {public: {gid: '1', size: '100'}}}\n"
		"  '5002': {config: {oslist: windows}, manifests: {public: {gid: '2', size: '100'}}}\n"
		"  '5003': {config: {oslist: linux}, manifests: {public: {gid: '3', size: '100'}}}\n");
	AppInfoProvision::pruneUnsupportedDepots(shared, 50, {true, 0});
	CHECK(!shared["depots"]["5003"] && AppInfoProvision::requiresProton(50),
	      "with the Linux depot dropped, shared untagged content does not make the game native");

	g_pLog.reset();
	std::string cleanup = std::string("rm -rf ") + home;
	(void)std::system(cleanup.c_str());
	return g_failures == 0 ? 0 : 1;
}
