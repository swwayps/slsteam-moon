// The owner drain runs inside Steam's IPC dispatch, so nothing it throws may
// unwind into Steam, and a failed frame must not leave the drain refused for
// the rest of the session.
// Build: make test-owner-frame-exceptions (links the whole object set).
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
#include <new>
#include <sstream>
#include <string>
#include <thread>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
// Once armed, the next allocation on this thread fails.
static thread_local bool t_failNextAllocation = false;
void* operator new(std::size_t size)
{
	if (t_failNextAllocation)
	{
		t_failNextAllocation = false;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_owner_frame_exceptions_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	HotReload::initialize();
	HotReload::publish({42});
	OwnerWork::latchOwnerThread();
	// A Proton title whose mapping is requested; a provisioning pass holds its
	// lock, so the publication is parked for a later owner frame.
	VFTIndexes::IClientCompat::GetCompatToolName.index = 0;
	VFTIndexes::IClientCompat::SpecifyCompatTool.index = 1;
	static void* compatVtable[] = {
		reinterpret_cast<void*>(+[](void*, AppId_t) -> const char* { return ""; }),
		reinterpret_cast<void*>(+[](void*, AppId_t, const char*, const char*, int32_t) {}),
	};
	static void** compatObject = compatVtable;
	g_pClientCompat = reinterpret_cast<IClientCompat*>(&compatObject);
	std::atomic<bool> held{false};
	std::atomic<bool> release{false};
	std::thread pass([&] {
		const std::lock_guard<std::mutex> lock(AppInfoProvision::provisioningPassMutex());
		held = true;
		while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	});
	while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	OwnerWork::submitCompatReadiness(42, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	// The frame that hands the parked publication back runs out of memory.
	bool escaped = false;
	t_failNextAllocation = true;
	try
	{
		OwnerWork::drainOnOwnerFrame();
	}
	catch (...)
	{
		escaped = true;
	}
	t_failNextAllocation = false;
	release = true;
	pass.join();
	CHECK(!escaped, "an exception in an owner frame does not unwind into Steam");
	// Work queued by another thread still drains on a later frame.
	std::thread([] { (void)OwnerWork::submitManagedState(PackageSnapshot{}); }).join();
	OwnerWork::drainOnOwnerFrame();
	std::stringstream log;
	log << std::ifstream(logPath).rdbuf();
	CHECK(log.str().find("drained 1 command(s)") != std::string::npos,
	      "the owner drain keeps working after a failed frame");
	g_pClientCompat = nullptr;
	if (g_failures == 0) std::printf("\nall owner frame exception checks passed\n");
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
