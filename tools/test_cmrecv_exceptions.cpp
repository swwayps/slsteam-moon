// Steam's CM receive hook hands every packet to CmRecv::dispatchNetPacket, so
// nothing a message handler throws may unwind into Steam.
// Build: make test-cmrecv-exceptions (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_vdf.hpp"
#include "../src/feats/cmrecv.hpp"
#include "../src/log.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/protobufs/steammessages_clientserver_appinfo.pb.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

// Once armed, the next allocation on this thread fails.
static thread_local bool t_failNextAllocation = false;
static thread_local bool t_allocationFailed = false;
// While set, bytes allocated on this thread are added up.
static thread_local bool t_countBytes = false;
static thread_local std::size_t t_allocatedBytes = 0;

void* operator new(std::size_t size)
{
	if (t_failNextAllocation)
	{
		t_failNextAllocation = false;
		t_allocationFailed = true;
		throw std::bad_alloc();
	}
	if (t_countBytes) t_allocatedBytes += size;
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_cmrecv_exceptions_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	// An empty product-info response, as the CM receive hook sees it.
	const std::string header = CMsgProtoBufHeader().SerializeAsString();
	const std::string message = CMsgClientPICSProductInfoResponse().SerializeAsString();
	std::vector<std::uint8_t> storage(sizeof(CNetPacketBody) + header.size() + message.size());
	auto* body = reinterpret_cast<CNetPacketBody*>(storage.data());
	body->type = CNetPacket::PROTOBUF_TYPE_MASK | k_EMsgClientPICSProductInfoResponse;
	body->headerSize = header.size();
	std::memcpy(storage.data() + sizeof(CNetPacketBody), header.data(), header.size());
	std::memcpy(storage.data() + sizeof(CNetPacketBody) + header.size(),
	            message.data(), message.size());
	CNetPacket packet{};
	packet.setBody(body);
	packet.setSize(storage.size());

	// The handler runs out of memory.
	bool escaped = false;
	t_failNextAllocation = true;
	try
	{
		CmRecv::dispatchNetPacket(&packet);
	}
	catch (...)
	{
		escaped = true;
	}
	t_failNextAllocation = false;
	CHECK(t_allocationFailed && !escaped,
	      "a handler that throws does not unwind into Steam's receive hook");

	// Most product-info answers are for other games: Steam's receive thread
	// must not copy them into a parsed message.
	{
		g_config.managedAppIds.set({42});
		CMsgClientPICSProductInfoResponse others;
		const std::string appinfo(4096, 'x');
		for (std::uint32_t app = 1000; app < 2000; ++app)
		{
			auto* entry = others.add_apps();
			entry->set_appid(app);
			entry->set_change_number(7);
			entry->set_buffer(appinfo);
		}
		const std::string large = others.SerializeAsString();
		std::vector<std::uint8_t> largeStorage(
			sizeof(CNetPacketBody) + header.size() + large.size());
		auto* largeBody = reinterpret_cast<CNetPacketBody*>(largeStorage.data());
		largeBody->type = CNetPacket::PROTOBUF_TYPE_MASK | k_EMsgClientPICSProductInfoResponse;
		largeBody->headerSize = header.size();
		std::memcpy(largeStorage.data() + sizeof(CNetPacketBody), header.data(), header.size());
		std::memcpy(largeStorage.data() + sizeof(CNetPacketBody) + header.size(),
		            large.data(), large.size());
		CNetPacket largePacket{};
		largePacket.setBody(largeBody);
		largePacket.setSize(largeStorage.size());
		t_allocatedBytes = 0;
		t_countBytes = true;
		CmRecv::dispatchNetPacket(&largePacket);
		t_countBytes = false;
		CHECK(t_allocatedBytes < 64 * 1024,
		      ("a 4 MiB answer without a managed app is not parsed (" +
		       std::to_string(t_allocatedBytes / 1024) + " KiB allocated)").c_str());
	}

	// The appinfo.vdf lookup skips a candidate it cannot resolve: here
	// ~/.steam/steam points at itself.
	const std::string dotSteam = std::string(home) + "/.steam";
	if (mkdir(dotSteam.c_str(), 0700) != 0 ||
	    symlink("steam", (dotSteam + "/steam").c_str()) != 0)
		return 2;
	const std::string appcache = std::string(home) + "/.local/share/Steam/appcache";
	std::filesystem::create_directories(appcache);
	std::ofstream(appcache + "/appinfo.vdf") << "x";
	std::string found;
	bool threw = false;
	try
	{
		found = AppInfoVdf::findExistingPath();
	}
	catch (...)
	{
		threw = true;
	}
	CHECK(!threw && found == appcache + "/appinfo.vdf",
	      "the appinfo.vdf lookup skips a path it cannot resolve");

	if (g_failures == 0) std::printf("\nall cmrecv exception checks passed\n");
	else std::printf("\n%d cmrecv exception check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
