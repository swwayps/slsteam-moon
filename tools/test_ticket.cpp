// Standalone test for ownership-ticket coverage.
//
// The network response must be stamped for both managed base apps and DLC
// ids registered from provisioned appinfo. Unknown apps remain untouched.
// Build from the repository root:
//   g++ -std=c++20 -I include tools/test_ticket.cpp -o /tmp/test_ticket && /tmp/test_ticket

#include "../src/config_discovery.hpp"
#include "../src/feats/ticket.hpp"
#include "../src/sdk/CSteamEngine.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace Ticket
{
	std::unordered_map<AppId_t, CSteamId> oneTimeSteamIdSpoof;
	std::unordered_map<AppId_t, SavedTicket> ticketMap;
	std::unordered_map<AppId_t, SavedTicket> encryptedTicketMap;
}

static_assert(std::is_same_v<decltype(&Ticket::forgetApp), bool (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(Ticket::SavedTicket::steamId), CSteamId>);
static_assert(sizeof(CSteamId) == sizeof(uint64_t));
static_assert(sizeof(CServerPipe) == 0x60);
static_assert(offsetof(CServerPipe, pipeHandle) == 0x8);
static_assert(offsetof(CServerPipe, pid) == 0x14);
static_assert(offsetof(CServerPipe, userHandle) == 0x21);

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	constexpr uint64_t fullSteamId = 76561198012345678ULL;
	const CSteamId identity(fullSteamId);
	CHECK(identity.isSet(), "ticket: full Steam identity is set");
	CHECK(identity.steamId64 == fullSteamId,
	      "ticket: full 64-bit Steam identity is preserved");

	CHECK(Ticket::shouldStampAppOwnershipTicket(true, false),
	      "ticket: managed base app is covered");
	CHECK(Ticket::shouldStampAppOwnershipTicket(false, true),
	      "ticket: registered DLC app is covered");
	CHECK(Ticket::shouldStampAppOwnershipTicket(true, true),
	      "ticket: base and DLC coverage compose");
	CHECK(!Ticket::shouldStampAppOwnershipTicket(false, false),
	      "ticket: unknown app remains untouched");

	Ticket::SavedTicket cached;
	cached.steamId = identity;
	cached.ticket = "ordinary";
	Ticket::ticketMap[400] = cached;
	Ticket::encryptedTicketMap[400] = cached;
	Ticket::ticketMap[401] = cached;
	CHECK(Ticket::forgetApp(400),
	      "ticket: forgetting an app clears both in-memory ticket caches");
	CHECK(Ticket::ticketMap.count(400) == 0 &&
	      Ticket::encryptedTicketMap.count(400) == 0,
	      "ticket: forgotten app is absent from both cache maps");
	CHECK(Ticket::isAppInvalidated(400),
	      "ticket: forgetting an app leaves a removal tombstone");
	CHECK(Ticket::restoreApp(400),
	      "ticket: a later hot-add clears the removal tombstone");
	CHECK(!Ticket::isAppInvalidated(400),
	      "ticket: restored app may use ticket caches again");
	CHECK(Ticket::ticketMap.count(401) == 1,
	      "ticket: forgetting one app preserves another app's ticket");
	CHECK(!Ticket::forgetApp(0), "ticket: zero app id is rejected");

	const CSteamId first(76561198000000001ULL);
	const CSteamId second(76561198000000002ULL);
	Ticket::oneTimeSteamIdSpoof[400] = first;
	Ticket::oneTimeSteamIdSpoof[401] = second;
	Ticket::oneTimeSteamIdSpoof.erase(400);
	CHECK(!Ticket::oneTimeSteamIdSpoof.contains(400),
	      "ticket: consuming one app's identity clears only that app");
	CHECK(Ticket::oneTimeSteamIdSpoof.at(401).steamId64 == second.steamId64,
	      "ticket: per-app identity state remains isolated");

	const std::unordered_set<uint32_t> before = {101, 202, 303};
	const std::unordered_set<uint32_t> after = {202, 404};
	const auto removed = ConfigDiscovery::removedAppIds(before, after);
	CHECK((removed == std::vector<uint32_t>{101, 303}),
	      "config watcher computes only removed app ids");

	std::ifstream configSource("src/config.cpp");
	const std::string configText((std::istreambuf_iterator<char>(configSource)),
	                            std::istreambuf_iterator<char>());
	CHECK(configText.find("ConfigDiscovery::removedAppIds") != std::string::npos,
	      "config watcher uses the removed-app diff helper");
	CHECK(configText.find("AppInfoProvision::forgetApp") != std::string::npos,
	      "config watcher invalidates native appinfo state on removal");
	CHECK(configText.find("Ticket::forgetApp") != std::string::npos,
	      "config watcher invalidates ticket state on removal");
	const auto manifestImport = configText.find("ManifestId::importLuaScripts();");
	const auto hotAddDetection = configText.find("bool hasNewApp = false;");
	CHECK(manifestImport != std::string::npos &&
	      hotAddDetection != std::string::npos &&
	      manifestImport < hotAddDetection,
	      "config watcher re-imports manifest pins for existing script edits");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
