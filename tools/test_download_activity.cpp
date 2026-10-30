// isAnyManagedDownloadActive asks Steam's app manager once it is resolved and
// reads appmanifest files only without it; the fleet scan calls Steam outside
// the cache lock, so a caller never waits for a scan that waits on Steam.
// Build: make test-download-activity (links the whole object set, as SLSsteam.so).
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/sdk/IClientAppManager.hpp"
#include "../src/utils/ManifestFetch.hpp"
#include "../src/vftableinfo.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
namespace
{
// The fake app manager reports every app as installed. The first call from the
// marked thread stays inside it until released (at most 3 s).
std::mutex g_mu;
std::condition_variable g_cv;
std::thread::id g_holdThread;
bool g_holding = false;
bool g_held = false;
bool g_released = false;

EAppState fakeGetAppInstallState(void*, AppId_t)
{
	std::unique_lock lock(g_mu);
	if (!g_held && std::this_thread::get_id() == g_holdThread)
	{
		g_held = true;
		g_holding = true;
		g_cv.notify_all();
		g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_released; });
		g_holding = false;
	}
	return k_EAppStateFullyInstalled;
}
void* g_vtable[1] = {reinterpret_cast<void*>(&fakeGetAppInstallState)};
struct FakeAppManager { void** vptr = g_vtable; } g_fake;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	char root[] = "/tmp/test_download_activity_XXXXXX";
	if (!mkdtemp(root)) return 2;
	const std::string steam = std::string(root) + "/.steam/steam";
	std::filesystem::create_directories(steam + "/steamapps");
	std::ofstream(steam + "/steam.sh") << "#!/bin/sh\n";
	// Written while the update ran; Steam has not rewritten it since.
	std::ofstream(steam + "/steamapps/appmanifest_42.acf")
		<< "\"AppState\"\n{\n\t\"appid\"\t\t\"42\"\n\t\"StateFlags\"\t\t\"1026\"\n}\n";
	setenv("HOME", root, 1);
	g_config.managedAppIds.set({42});
	VFTIndexes::IClientAppManager::GetAppInstallState.index = 0;
	g_pClientAppManager = reinterpret_cast<IClientAppManager*>(&g_fake);

	// One caller's fleet scan is inside Steam's app manager; a second caller
	// asking meanwhile must not wait for it.
	std::thread scanner([] {
		{
			std::lock_guard lock(g_mu);
			g_holdThread = std::this_thread::get_id();
		}
		(void)ManifestFetch::isAnyManagedDownloadActive(0);
	});
	{
		std::unique_lock lock(g_mu);
		g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_held; });
	}
	const auto start = std::chrono::steady_clock::now();
	(void)ManifestFetch::isAnyManagedDownloadActive(0);
	const auto waited = std::chrono::steady_clock::now() - start;
	bool stillHolding = false;
	{
		std::lock_guard lock(g_mu);
		stillHolding = g_holding;
		g_released = true;
	}
	g_cv.notify_all();
	scanner.join();
	CHECK(stillHolding && waited < std::chrono::seconds(1),
	      "a caller does not wait for a scan that is inside Steam");

	CHECK(!ManifestFetch::isAnyManagedDownloadActive(42),
	      "a resolved app manager answers without the appmanifest");
	g_pClientAppManager = nullptr;
	CHECK(ManifestFetch::isAnyManagedDownloadActive(42),
	      "without the app manager the appmanifest answers");

	std::filesystem::remove_all(root);
	return g_failures == 0 ? 0 : 1;
}
