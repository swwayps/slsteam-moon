// Regression test for the SteamStub launch path (make test-steamstub-launch).
// It runs inside Steam's LaunchApp hook, so a filesystem error while looking up
// the install folder must be skipped instead of thrown into Steam.
#include "../src/config.hpp"
#include "../src/feats/steamstub.hpp"
#include "../src/log.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
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
// setenv() as seen by the linked SteamStub code: a call from a forked child
// (not async-signal-safe there, it allocates) leaves a marker file.
static pid_t g_parentPid = getpid();
static char g_childSetenvMark[256] = "";
extern "C" int setenv(const char* name, const char* value, int overwrite)
{
	if (getpid() != g_parentPid && g_childSetenvMark[0] != '\0')
	{
		const int fd = open(g_childSetenvMark, O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) close(fd);
	}
	using Real = int (*)(const char*, const char*, int);
	static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "setenv"));
	return real(name, value, overwrite);
}
namespace
{
	// Writes a file that carries the SteamStub v2 magic at 0x40.
	void writeStubExe(const std::string& path)
	{
		std::string bytes(0x40, '\0');
		bytes[0] = 'M';
		bytes[1] = 'Z';
		bytes.append("VLV", 4);
		std::ofstream(path, std::ios::binary) << bytes;
	}

	// The opt-in launches run in a fresh process: the tool lookup is cached for
	// the process, and the parent later runs without the tools.
	int prewarmChild()
	{
		namespace fs = std::filesystem;
		const std::string root = std::getenv("HOME");
		const std::string steam = root + "/.local/share/Steam";
		g_pLog = std::unique_ptr<CLog>(new CLog((root + "/child.log").c_str()));
		const std::string bin = root + "/bin";
		fs::create_directories(bin);
		for (const char* tool : {"python3", "timeout"})
		{
			std::ofstream(bin + "/" + tool) << "#!/bin/sh\n";
			fs::permissions(bin + "/" + tool, fs::perms::owner_all);
		}
		const char* inherited = std::getenv("PATH");
		setenv("PATH", (bin + ":" + (inherited ? inherited : "")).c_str(), 1);

		// The recording helper fails --prewarm while fail-prewarm exists.
		const std::string kit = root + "/kit";
		const std::string calls = root + "/helper.calls";
		fs::create_directories(kit + "/tools/steamstub-bypass");
		fs::create_directories(kit + "/tools/steamless-bin");
		std::ofstream(kit + "/tools/steamless-bin/Steamless.CLI.exe") << "";
		std::ofstream(kit + "/tools/steamstub-bypass/run-steamless.sh")
			<< "printf '%s:%s:%s\\n' \"$STEAMLESS_HOME\" \"$QUIET\" \"$1\" >> " << calls << "\n"
			<< "[ \"$1\" = --prewarm ] && [ -e " << root << "/fail-prewarm ] && exit 9\n"
			<< "exit 0\n";
		std::snprintf(g_childSetenvMark, sizeof(g_childSetenvMark), "%s/child-setenv", root.c_str());
		SteamStub::setup(kit.c_str());

		const std::string game = steam + "/steamapps/common/Stub";
		fs::create_directories(game);
		std::ofstream(steam + "/steamapps/appmanifest_124.acf")
			<< "\"AppState\"\n{\n\t\"installdir\"\t\t\"Stub\"\n}\n";
		fs::create_directories(steam + "/userdata/1/config");
		std::ofstream(steam + "/userdata/1/config/localconfig.vdf")
			<< "\"apps\"\n{\n\t\"124\"\n\t{\n\t\t\"LaunchOptions\"\t\t\"--steamless %command%\"\n\t}\n}\n";
		g_config.addedAppIds.set({124});

		std::ofstream(root + "/fail-prewarm") << "";
		writeStubExe(game + "/A.exe");
		SteamStub::onLaunchApp(124);
		fs::remove(root + "/fail-prewarm");
		writeStubExe(game + "/B.exe");
		SteamStub::onLaunchApp(124);
		writeStubExe(game + "/C.exe");
		SteamStub::onLaunchApp(124);

		std::ifstream file(calls);
		const std::string seen((std::istreambuf_iterator<char>(file)), {});
		const std::string home = kit + "/tools/steamless-bin:1:";
		check(seen == home + "--prewarm\n" + home + game + "/A.exe\n"
		              + home + "--prewarm\n" + home + game + "/B.exe\n"
		              + home + game + "/C.exe\n",
		      "the first opt-in launch prepares the prefix once before the exe, a failed "
		      "preparation is tried again by the next launch, a ready prefix is not "
		      "prepared again, and the helper sees STEAMLESS_HOME and QUIET (got \""
		      + seen + "\")");
		check(!fs::exists(g_childSetenvMark), "the forked child does not call setenv");
		return failures == 0 ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if (argc > 1 && std::string(argv[1]) == "prewarm-child") return prewarmChild();
	const std::string root = "/tmp/slssteam-steamstub-launch." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	const std::string steam = root + "/.local/share/Steam";
	std::filesystem::create_directories(configHome + "/SLSsteam");
	std::filesystem::create_directories(steam + "/steamapps");
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	// A library folder that cannot be resolved (a symlink loop fails with
	// ELOOP, like an unreachable mount) sits before the real library.
	std::filesystem::create_directory_symlink(root + "/loop", root + "/loop");
	std::ofstream(steam + "/steamapps/libraryfolders.vdf")
		<< "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << root
		<< "/loop\"\n\t}\n}\n";
	g_config.addedAppIds.set({123});

	bool threw = false;
	try { SteamStub::onLaunchApp(123); }
	catch (...) { threw = true; }
	check(!threw, "an unresolvable library folder does not throw out of the launch hook");

	// The helper gets its environment from the parent; the forked child only
	// execs (a setenv there can deadlock on a lock another thread held).
	{
		const std::string childHome = root + "/prewarm-home";
		std::filesystem::create_directories(childHome);
		const pid_t pid = fork();
		if (pid == 0)
		{
			setenv("HOME", childHome.c_str(), 1);
			setenv("XDG_CONFIG_HOME", (childHome + "/config").c_str(), 1);
			std::filesystem::create_directories(childHome + "/config/SLSsteam");
			execl("/proc/self/exe", "test_steamstub_launch", "prewarm-child", nullptr);
			_exit(127);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "the opt-in launch sequence passes in a fresh process");
	}

	// The remaining cases set the helper kit up in this process.
	const std::string kit = root + "/kit";
	std::filesystem::create_directories(kit + "/tools/steamstub-bypass");
	std::filesystem::create_directories(kit + "/tools/steamless-bin");
	std::ofstream(kit + "/tools/steamless-bin/Steamless.CLI.exe") << "";
	std::ofstream(kit + "/tools/steamstub-bypass/run-steamless.sh") << "exit 0\n";
	SteamStub::setup(kit.c_str());

	// Without --steamless a launch reverts only our own fresh output.
	{
		namespace fs = std::filesystem;
		const std::string game = steam + "/steamapps/common/Game";
		fs::create_directories(game);
		std::ofstream(steam + "/steamapps/appmanifest_123.acf")
			<< "\"AppState\"\n{\n\t\"installdir\"\t\t\"Game\"\n}\n";
		const auto readAll = [](const std::string& path) {
			std::ifstream file(path);
			return std::string((std::istreambuf_iterator<char>(file)), {});
		};
		const auto hourAgo = fs::file_time_type::clock::now() - std::chrono::hours(1);
		// Steam updated the game after Steamless ran: the backup is stale.
		std::ofstream(game + "/Game.exe") << "MZnew";
		std::ofstream(game + "/Game.exe.original.exe") << "MZold";
		std::ofstream(game + "/Game.exe.steamless_done") << "";
		fs::last_write_time(game + "/Game.exe.original.exe", hourAgo);
		fs::last_write_time(game + "/Game.exe.steamless_done", hourAgo);
		// A Steamless result: the helper writes the marker right after it
		// swaps Steamless's fresh output in.
		std::ofstream(game + "/Tool.exe") << "MZstripped";
		std::ofstream(game + "/Tool.exe.original.exe") << "MZorig";
		std::ofstream(game + "/Tool.exe.steamless_done") << "";
		fs::last_write_time(game + "/Tool.exe", hourAgo);
		fs::last_write_time(game + "/Tool.exe.steamless_done", hourAgo + std::chrono::seconds(2));
		// A fix replaced our output after Steamless ran; extracting it kept
		// the archive's time, which is older than the marker.
		std::ofstream(game + "/Fix.exe") << "MZfix";
		std::ofstream(game + "/Fix.exe.original.exe") << "MZstub";
		std::ofstream(game + "/Fix.exe.steamless_done") << "";
		fs::last_write_time(game + "/Fix.exe", hourAgo - std::chrono::hours(1));
		fs::last_write_time(game + "/Fix.exe.steamless_done", hourAgo);
		// A file of the game itself that happens to end in .original.exe.
		std::ofstream(game + "/Data.original.exe") << "MZgame";
		SteamStub::onLaunchApp(123);
		check(readAll(game + "/Game.exe") == "MZnew" && !fs::exists(game + "/Game.exe.original.exe"),
		      "a backup older than the live exe is dropped, not restored over it");
		check(readAll(game + "/Tool.exe") == "MZorig",
		      "a Steamless result is reverted");
		check(readAll(game + "/Fix.exe") == "MZfix" && !fs::exists(game + "/Fix.exe.original.exe") &&
		          !fs::exists(game + "/Fix.exe.steamless_done"),
		      "an exe replaced after Steamless ran stays, even with an older time");
		check(fs::exists(game + "/Data.original.exe") && !fs::exists(game + "/Data"),
		      "a file without our marker is left alone");
	}

	// Only --steamless needs python3 and timeout, so they are looked up when a
	// launch asks for it: one notice per session, none for other launches.
	{
		namespace fs = std::filesystem;
		const auto missingNotices = [&] {
			std::ifstream file(root + "/test.log");
			const std::string text((std::istreambuf_iterator<char>(file)), {});
			std::size_t count = 0;
			for (auto at = text.find("Runtime dependencies missing"); at != std::string::npos;
			     at = text.find("Runtime dependencies missing", at + 1))
				++count;
			return count;
		};
		const std::string bare = root + "/bare-path";
		fs::create_directories(bare);
		const char* inherited = std::getenv("PATH");
		const std::string savedPath = inherited ? inherited : "";
		setenv("PATH", bare.c_str(), 1);
		SteamStub::onLaunchApp(123);
		check(missingNotices() == 0, "a launch without --steamless looks up no tools");
		fs::create_directories(steam + "/userdata/1/config");
		std::ofstream(steam + "/userdata/1/config/localconfig.vdf")
			<< "\"apps\"\n{\n\t\"123\"\n\t{\n\t\t\"LaunchOptions\"\t\t\"--steamless %command%\"\n\t}\n}\n";
		SteamStub::onLaunchApp(123);
		SteamStub::onLaunchApp(123);
		check(missingNotices() == 1, "a --steamless launch without python3 warns once per session");
		setenv("PATH", savedPath.c_str(), 1);
	}

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_steamstub_launch: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
