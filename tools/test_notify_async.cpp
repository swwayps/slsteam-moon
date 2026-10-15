// notifyUser runs on Steam threads (PICS, install paths), so a notification
// daemon that does not answer must not hold the caller.
// Build: make test-notify-async
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/usermsg.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_notify_async_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	// A notify-send that records the call and then hangs, like a stuck daemon.
	const std::string bin = std::string(home) + "/bin";
	std::filesystem::create_directories(bin);
	const std::string marker = std::string(home) + "/shown";
	std::ofstream(bin + "/notify-send") << "#!/bin/sh\ntouch '" << marker << "'\nsleep 5\n";
	std::filesystem::permissions(bin + "/notify-send", std::filesystem::perms::owner_all);
	setenv("PATH", (bin + ":" + std::getenv("PATH")).c_str(), 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	g_config.notifications.set(true);

	const auto start = std::chrono::steady_clock::now();
	g_pLog->notifyUser(UserMsg::ConfigWriteFailed);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed < std::chrono::seconds(1),
	      "notifyUser returns while the notification is still pending");
	for (int i = 0; i < 40 && !std::filesystem::exists(marker); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(std::filesystem::exists(marker), "the notification is still sent");

	if (g_failures == 0) std::printf("\nall notify checks passed\n");
	else std::printf("\n%d notify check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
