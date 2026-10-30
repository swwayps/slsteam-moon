// Offline flags live in the config folder (XDG_CONFIG_HOME aware) and are
// cleared by load(), not when the library image loads: a second steam process
// (a steam:// handoff) must not clear the running session's flags, and an
// unusable config directory must not abort it. A game's flag follows its
// depots; the global flag changes only when the archive circuit opens or closes.
// Build: make test-offline-flags (links the whole object set, as SLSsteam.so).
#include "../src/log.hpp"
#include "../src/utils/ManifestFetch.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

// Counts removals of the global flag: the library's std::remove resolves here.
static std::atomic<int> g_offlineRemoves{0};
extern "C" int remove(const char* path) noexcept
{
	if (path && std::string_view(path).ends_with("/offline"))
		g_offlineRemoves.fetch_add(1);
	int rc = unlink(path);
	if (rc != 0 && errno == EISDIR) rc = rmdir(path);
	return rc;
}

namespace
{
namespace fs = std::filesystem;

// Runs this binary again so the image loads with that HOME (and
// XDG_CONFIG_HOME when given).
int runChild(const std::string& home, const char* mode, const std::string& xdg = {})
{
	const pid_t pid = fork();
	if (pid == 0)
	{
		setenv("HOME", home.c_str(), 1);
		if (xdg.empty()) unsetenv("XDG_CONFIG_HOME");
		else setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
		execl("/proc/self/exe", "test_offline_flags", "child", mode, nullptr);
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

std::string env(const char* name) { return std::getenv(name) ? std::getenv(name) : ""; }

int flagChild()
{
	const std::string flag = env("HOME") + "/.config/SLSsteam/offline_42";
	if (!fs::exists(flag)) return 3;
	ManifestFetch::clearOfflineFlags();
	return fs::exists(flag) ? 4 : 0;
}

int xdgChild()
{
	const std::string dir = env("XDG_CONFIG_HOME") + "/SLSsteam";
	const std::string legacy = env("HOME") + "/.config/SLSsteam/offline";
	ManifestFetch::clearOfflineFlags();
	CHECK(!fs::exists(dir + "/offline") && !fs::exists(dir + "/offline_42"),
	      "clearOfflineFlags clears the flags in XDG_CONFIG_HOME");
	CHECK(!fs::exists(legacy),
	      "clearOfflineFlags also drops the global flag an older version kept below HOME");
	return g_failures;
}

int depotsChild()
{
	const std::string flag = env("XDG_CONFIG_HOME") + "/SLSsteam/offline_42";
	ManifestFetch::setDepotOnFallback(42, 1, true);
	CHECK(fs::exists(flag), "a depot installed from a local manifest flags its game");
	ManifestFetch::setDepotOnFallback(42, 2, true);
	ManifestFetch::setDepotOnFallback(42, 1, false);
	CHECK(fs::exists(flag), "the game stays flagged while another depot is on a local manifest");
	ManifestFetch::setDepotOnFallback(42, 2, false);
	CHECK(!fs::exists(flag), "the flag goes once every depot got its exact manifest");
	ManifestFetch::setDepotOnFallback(42, 2, true);
	fs::remove(flag);
	ManifestFetch::setDepotOnFallback(42, 2, true);
	CHECK(!fs::exists(flag), "a depot already flagged writes nothing again");
	ManifestFetch::clearOfflineFlags();
	ManifestFetch::setDepotOnFallback(42, 2, true);
	CHECK(fs::exists(flag), "after the cleanup a depot on a local manifest flags its game again");
	return g_failures;
}

int circuitChild()
{
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	const std::string home = env("HOME");
	fs::create_directories(home + "/.steam/steam/depotcache");
	std::ofstream(home + "/.steam/steam/steam.sh") << "#!/bin/sh\n";
	// Every archive request is refused: no proxy listens on the discard port.
	setenv("https_proxy", "http://127.0.0.1:9", 1);
	setenv("ALL_PROXY", "http://127.0.0.1:9", 1);
	unsetenv("NO_PROXY");
	unsetenv("no_proxy");
	// One transport failure leaves the circuit closed, the second opens it.
	(void)ManifestFetch::awaitManifestBlobFor(111, 7, 15000, false, false);
	CHECK(g_offlineRemoves.load() == 0,
	      "an attempt that leaves the circuit closed does not touch the offline flag");
	(void)ManifestFetch::awaitManifestBlobFor(222, 7, 15000, false, false);
	CHECK(fs::exists(env("XDG_CONFIG_HOME") + "/SLSsteam/offline"),
	      "an opened circuit sets the offline flag in the config folder");
	return g_failures;
}
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc > 2 && std::string(argv[1]) == "child")
	{
		const std::string mode = argv[2];
		if (mode == "loop") return 0;
		if (mode == "flag") return flagChild();
		if (mode == "xdg") return xdgChild();
		if (mode == "depots") return depotsChild();
		if (mode == "circuit") return circuitChild();
		return 126;
	}
	char root[] = "/tmp/test_offline_flags_XXXXXX";
	if (!mkdtemp(root)) return 2;

	const std::string flagged = std::string(root) + "/flagged";
	fs::create_directories(flagged + "/.config/SLSsteam");
	std::ofstream(flagged + "/.config/SLSsteam/offline_42") << "1\n";
	const int flaggedRc = runChild(flagged, "flag");
	CHECK(flaggedRc != 3, "loading the library keeps the session's offline flags");
	CHECK(flaggedRc == 0, "clearOfflineFlags removes them");

	// .config resolves to itself, so every lookup below it fails with ELOOP.
	const std::string looped = std::string(root) + "/looped";
	fs::create_directories(looped);
	fs::create_directory_symlink(".config", looped + "/.config");
	CHECK(runChild(looped, "loop") == 0,
	      "an unusable config directory does not abort the process at load");

	// The child processes print their own checks; a nonzero status is a failure.
	const std::string xdgHome = std::string(root) + "/xdg-home";
	const std::string xdg = xdgHome + "/xdg";
	fs::create_directories(xdg + "/SLSsteam");
	fs::create_directories(xdgHome + "/.config/SLSsteam");
	std::ofstream(xdg + "/SLSsteam/offline") << "1\n";
	std::ofstream(xdg + "/SLSsteam/offline_42") << "1\n";
	std::ofstream(xdgHome + "/.config/SLSsteam/offline") << "1\n";
	CHECK(runChild(xdgHome, "xdg", xdg) == 0, "flag cleanup follows XDG_CONFIG_HOME");

	const std::string depotHome = std::string(root) + "/depot-home";
	fs::create_directories(depotHome + "/xdg/SLSsteam");
	CHECK(runChild(depotHome, "depots", depotHome + "/xdg") == 0,
	      "a game's flag follows its depots");

	const std::string circuitHome = std::string(root) + "/circuit-home";
	fs::create_directories(circuitHome + "/xdg/SLSsteam");
	fs::create_directories(circuitHome + "/.config/SLSsteam");
	CHECK(runChild(circuitHome, "circuit", circuitHome + "/xdg") == 0,
	      "the global flag follows the archive circuit");

	fs::remove_all(root);
	return g_failures == 0 ? 0 : 1;
}
