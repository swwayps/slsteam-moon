// The PICS callbacks run on Steam's CM receive thread. What they ask the
// provisioner must not wait for a provisioning pass, which holds its mutex
// across fleet-wide disk work.
// Build: make test-provision-locks (links the whole object set).
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/log.hpp"
#include "../src/ownerwork.hpp"
#include "../src/sdk/IClientCompat.hpp"
#include "../src/vftableinfo.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

// Runs `call` on its own thread; true when it returned within two seconds.
template <typename Call>
bool returnsPromptly(Call call)
{
	const auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread([done, call] {
		call();
		done->store(true);
	}).detach();
	for (int i = 0; i < 200 && !done->load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return done->load();
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_provision_locks_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	HotReload::initialize();
	HotReload::publish({42}, true);

	// The owner IPC thread publishes a Proton title after its mapping. A pass
	// holding its mutex must not stall that thread; the publication waits for
	// a later owner frame instead.
	OwnerWork::latchOwnerThread();
	VFTIndexes::IClientCompat::GetCompatToolName.index = 0;
	VFTIndexes::IClientCompat::SpecifyCompatTool.index = 1;
	static void* compatVtable[] = {
		reinterpret_cast<void*>(+[](void*, AppId_t) -> const char* { return "proton_experimental"; }),
		reinterpret_cast<void*>(+[](void*, AppId_t, const char*, const char*, int32_t) {}),
	};
	static void** compatObject = compatVtable;
	g_pClientCompat = reinterpret_cast<IClientCompat*>(&compatObject);
	std::atomic<bool> passHeld{false};
	std::thread pass([&] {
		const std::lock_guard<std::mutex> lock(AppInfoProvision::provisioningPassMutex());
		passHeld = true;
		std::this_thread::sleep_for(std::chrono::seconds(2));
	});
	while (!passHeld) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const auto submitted = std::chrono::steady_clock::now();
	OwnerWork::submitCompatReadiness(42, 0);
	CHECK(std::chrono::steady_clock::now() - submitted < std::chrono::seconds(1),
	      "the owner thread does not wait for a provisioning pass to publish");
	pass.join();
	OwnerWork::drainOnOwnerFrame();
	std::stringstream log;
	log << std::ifstream(logPath).rdbuf();
	CHECK(log.str().find("prepared base=42 not published") != std::string::npos,
	      "the publication runs on the next owner frame");

	// A busy UI runs owner frames much faster than a pass holds its lock.
	// Frames inside one hold must not use up the retry budget, and a retry
	// must not specify the mapping again.
	static std::atomic<int> specifyCalls{0};
	static void* requestedVtable[] = {
		reinterpret_cast<void*>(+[](void*, AppId_t) -> const char* { return ""; }),
		reinterpret_cast<void*>(+[](void*, AppId_t, const char*, const char*, int32_t) {
			++specifyCalls;
		}),
	};
	static void** requestedObject = requestedVtable;
	g_pClientCompat = reinterpret_cast<IClientCompat*>(&requestedObject);
	const auto notPublishedLines = [&logPath] {
		std::stringstream text;
		text << std::ifstream(logPath).rdbuf();
		std::size_t count = 0;
		for (std::size_t at = text.str().find("prepared base=42 not published");
		     at != std::string::npos;
		     at = text.str().find("prepared base=42 not published", at + 1))
			++count;
		return count;
	};
	const std::size_t before = notPublishedLines();
	std::atomic<bool> holdStarted{false};
	std::thread hold([&] {
		const std::lock_guard<std::mutex> lock(AppInfoProvision::provisioningPassMutex());
		holdStarted = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	});
	while (!holdStarted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	OwnerWork::submitCompatReadiness(42, 0);
	for (int frame = 0; frame < 25; ++frame)
	{
		OwnerWork::drainOnOwnerFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	hold.join();
	for (int frame = 0; frame < 200 && notPublishedLines() == before; ++frame)
	{
		OwnerWork::drainOnOwnerFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::stringstream busyLog;
	busyLog << std::ifstream(logPath).rdbuf();
	CHECK(busyLog.str().find("stayed busy for app=42") == std::string::npos &&
	          notPublishedLines() == before + 1,
	      "owner frames inside one lock hold do not use up the retry budget");
	CHECK(specifyCalls.load() == 1,
	      "a busy retry does not specify the mapping again");
	g_pClientCompat = nullptr;

	// A provisioning pass is running.
	AppInfoProvision::provisioningPassMutex().lock();
	CHECK(returnsPromptly([] { (void)AppInfoProvision::snapshotCachePublication(42); }),
	      "a publication snapshot does not wait for a provisioning pass");
	CHECK(returnsPromptly([] { (void)AppInfoProvision::observeTerminal(42, 1); }),
	      "a terminal lookup does not wait for a provisioning pass");
	AppInfoProvision::provisioningPassMutex().unlock();

	// A HotReload publication holds its coordinator while it waits here.
	AppInfoProvision::cachePublicationMutex().lock();
	std::thread([] { HotReload::publish({42, 43}, true); }).detach();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	CHECK(returnsPromptly([] { (void)HotReload::takeMissingCacheRepairRequests(); }),
	      "cache repair scheduling does not wait for a HotReload publication");
	CHECK(returnsPromptly([] { HotReload::repairMissingDlcMetadata("/nonexistent/appinfo.vdf"); }),
	      "DLC metadata repair scheduling does not wait for a HotReload publication");

	if (g_failures == 0) std::printf("\nall provision lock checks passed\n");
	else std::printf("\n%d provision lock check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	// A blocked call still holds its thread, so skip static destructors.
	std::_Exit(g_failures == 0 ? 0 : 1);
}
