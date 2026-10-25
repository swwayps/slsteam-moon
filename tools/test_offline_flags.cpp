// Offline flags are cleared by load(), not when the library image loads: a
// second steam process (a steam:// handoff) must not clear the running
// session's flags, and an unusable config directory must not abort it.
// Build: make test-offline-flags (links the whole object set, as SLSsteam.so).
#include "../src/utils/ManifestFetch.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
namespace
{
// Runs this binary again with HOME=home so the image loads with that HOME.
int runChild(const std::string& home, const char* mode)
{
	const pid_t pid = fork();
	if (pid == 0)
	{
		setenv("HOME", home.c_str(), 1);
		execl("/proc/self/exe", "test_offline_flags", "child", mode, nullptr);
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
}
int main(int argc, char** argv)
{
	if (argc > 2 && std::string(argv[1]) == "child")
	{
		if (std::string(argv[2]) == "loop") return 0;
		const std::string flag =
			std::string(std::getenv("HOME")) + "/.config/SLSsteam/offline_42";
		if (!std::filesystem::exists(flag)) return 3;
		ManifestFetch::clearOfflineFlags();
		return std::filesystem::exists(flag) ? 4 : 0;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	char root[] = "/tmp/test_offline_flags_XXXXXX";
	if (!mkdtemp(root)) return 2;

	const std::string flagged = std::string(root) + "/flagged";
	std::filesystem::create_directories(flagged + "/.config/SLSsteam");
	std::ofstream(flagged + "/.config/SLSsteam/offline_42") << "1\n";
	const int flaggedRc = runChild(flagged, "flag");
	CHECK(flaggedRc != 3, "loading the library keeps the session's offline flags");
	CHECK(flaggedRc == 0, "clearOfflineFlags removes them");

	// .config resolves to itself, so every lookup below it fails with ELOOP.
	const std::string looped = std::string(root) + "/looped";
	std::filesystem::create_directories(looped);
	std::filesystem::create_directory_symlink(".config", looped + "/.config");
	CHECK(runChild(looped, "loop") == 0,
	      "an unusable config directory does not abort the process at load");

	std::filesystem::remove_all(root);
	return g_failures == 0 ? 0 : 1;
}
