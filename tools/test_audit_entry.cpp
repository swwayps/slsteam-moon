// The rtld-audit entry points, driven the way the loader calls them: setup()
// through la_preinit, then la_objopen for steamui.so. Each case runs in a fresh
// process. libmem's process and module lookups are wrapped, and an allocation
// can be made to fail, so the guards around setup(), the decompiler pass and
// load() are exercised at run time instead of read from main.cpp.
// Build: make test-audit-entry (links the whole object set, as SLSsteam.so).
#include "../src/utils/process_lock.hpp"
#include "libmem/libmem.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <link.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void la_preinit(uintptr_t* cookie);
extern "C" unsigned int la_objopen(struct link_map* map, Lmid_t lmid, uintptr_t* cookie);

namespace
{
// Allocations on this thread that fail next.
thread_local int t_failNext = 0;

enum class Case
{
	SteamUiOnly,   // only steamui.so is mapped yet
	ParseThrows,   // the decompiler pass throws
	BothModules,   // load() runs on, and finds no Steam allocator export
	SetupThrows,   // setup() and its handler's log line both throw
	LoadThrows,    // load() and its handler's log line both throw
	Secondary,     // another namespace holds the setup lock
};
Case g_case = Case::SteamUiOnly;
bool g_armed = false;

bool fakeModule(const char* name, lm_module_t* out)
{
	const bool steamUi = std::strcmp(name, "steamui.so") == 0;
	const bool client = std::strcmp(name, "steamclient.so") == 0;
	if (!steamUi && !client) return false;
	if (client && g_case != Case::BothModules && g_case != Case::LoadThrows) return false;
	*out = {};
	out->base = client ? 0x20000000 : 0x30000000;
	out->size = 0x1000;
	out->end = out->base + out->size;
	std::snprintf(out->path, sizeof out->path, "/x/%s", name);
	std::snprintf(out->name, sizeof out->name, "%s", name);
	// Fail the next allocation(s) right before the guarded work: the first
	// steamui.so lookup comes from the decompiler pass, the first
	// steamclient.so lookup from load().
	if (!g_armed && ((g_case == Case::ParseThrows && steamUi) ||
	                 (g_case == Case::LoadThrows && client)))
	{
		g_armed = true;
		t_failNext = g_case == Case::ParseThrows ? 1 : 2;
	}
	return true;
}
} // namespace

void* operator new(std::size_t size)
{
	if (t_failNext > 0)
	{
		--t_failNext;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

extern "C" char* __real_getenv(const char* name);
extern "C" char* __wrap_getenv(const char* name)
{
	// setup() reads this right after the log exists: the next two
	// allocations (the setup lock path, then the handler's log line) fail.
	if (g_case == Case::SetupThrows && !g_armed && name &&
	    std::strcmp(name, "SLSSTEAM_AUDIT_NARROW") == 0)
	{
		g_armed = true;
		t_failNext = 2;
	}
	return __real_getenv(name);
}

extern "C" lm_bool_t __wrap_LM_GetProcess(lm_process_t* out)
{
	*out = {};
	out->pid = getpid();
	std::snprintf(out->name, sizeof out->name, "steam");
	return LM_TRUE;
}

extern "C" lm_bool_t __wrap_LM_FindModule(lm_string_t name, lm_module_t* out)
{
	return name && out && fakeModule(name, out) ? LM_TRUE : LM_FALSE;
}

namespace
{
int failures = 0;
void check(bool condition, const std::string& message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message.c_str());
	if (!condition) ++failures;
}

std::string readAll(const std::string& path)
{
	std::ifstream file(path);
	return std::string(std::istreambuf_iterator<char>(file), {});
}

std::size_t count(const std::string& text, const std::string& needle)
{
	std::size_t n = 0;
	for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
		++n;
	return n;
}

int runCase(const char* name)
{
	const std::string mode = name;
	if (mode == "steamui-only") g_case = Case::SteamUiOnly;
	else if (mode == "parse-throws") g_case = Case::ParseThrows;
	else if (mode == "both-modules") g_case = Case::BothModules;
	else if (mode == "setup-throws") g_case = Case::SetupThrows;
	else if (mode == "load-throws") g_case = Case::LoadThrows;
	else if (mode == "secondary") g_case = Case::Secondary;
	else return 126;

	if (g_case == Case::Secondary)
	{
		// The first namespace's lock: setup() must skip, yet the narrowed
		// object policy it reads from the environment still applies here.
		setenv("SLSSTEAM_AUDIT_NARROW", "1", 1);
		static ProcessLock::FileLock held(ProcessLock::perProcessPath("setup"));
		if (!held.acquired()) return 125;
		la_preinit(nullptr);
		char libm[] = "/lib/x86_64-linux-gnu/libm.so.6";
		struct link_map map{};
		map.l_name = libm;
		std::_Exit(static_cast<int>(la_objopen(&map, LM_ID_BASE, nullptr)));
	}
	la_preinit(nullptr);
	char steamUi[] = "/x/steamui.so";
	struct link_map map{};
	map.l_name = steamUi;
	(void)la_objopen(&map, LM_ID_BASE, nullptr);
	// Threads setup() started stay running: skip the static destructors.
	std::_Exit(0);
}

struct Result
{
	int status = -1;
	std::string log;
};

// Runs one case in a fresh process with its own HOME and config folders.
Result run(const std::string& root, const char* mode)
{
	const std::string home = root + "/" + mode;
	std::filesystem::create_directories(home + "/config");
	mkdir((home + "/run").c_str(), 0700);
	std::fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0)
	{
		setenv("HOME", home.c_str(), 1);
		setenv("XDG_CONFIG_HOME", (home + "/config").c_str(), 1);
		setenv("XDG_RUNTIME_DIR", (home + "/run").c_str(), 1);
		setenv("container", "pressure-vessel", 1);
		unsetenv("SLSSTEAM_AUDIT_NARROW");
		unsetenv("SLSSTEAM_AUDIT_BINDALL");
		execl("/proc/self/exe", "test_audit_entry", "case", mode, nullptr);
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
	        readAll(home + "/.SLSsteam.log")};
}
} // namespace

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc > 2 && std::string(argv[1]) == "case") return runCase(argv[2]);
	char root[] = "/tmp/test_audit_entry_XXXXXX";
	if (!mkdtemp(root)) return 2;
	const auto status = [](const Result& r) { return " (exit " + std::to_string(r.status) + ")"; };

	// steamclient.so and steamui.so open in either order, so load() runs once
	// with the other one unmapped and must just wait for the next la_objopen.
	// Tearing down there stopped the manifest donor for the whole session.
	const Result uiOnly = run(root, "steamui-only");
	check(uiOnly.status == 0 && uiOnly.log.find("Failed to open /x/steamui.so") != std::string::npos &&
	          uiOnly.log.find("Hooks::remove:") == std::string::npos &&
	          uiOnly.log.find("claimed hooking pass") == std::string::npos,
	      "with only steamui.so mapped, load() waits without tearing anything down" + status(uiOnly));

	// No exception may leave an rtld-audit callback.
	const Result parse = run(root, "parse-throws");
	check(parse.status == 0 &&
	          parse.log.find("decompiler failed on a client module") != std::string::npos,
	      "a decompiler pass that throws is contained in la_objopen" + status(parse));

	const Result setupThrows = run(root, "setup-throws");
	check(setupThrows.status == 0 && setupThrows.log.find("Hooks::remove:") != std::string::npos,
	      "setup() that throws, with its handler's log line throwing too, stays in la_preinit" +
	          status(setupThrows));

	const Result loadThrows = run(root, "load-throws");
	check(loadThrows.status == 0 && loadThrows.log.find("Hooks::remove:") != std::string::npos &&
	          loadThrows.log.find("claimed hooking pass") == std::string::npos,
	      "load() that throws, with its handler's log line throwing too, tears down inside "
	      "la_objopen" + status(loadThrows));

	// A load() exit that leaves Steam unmodified tells the user.
	const Result both = run(root, "both-modules");
	const std::size_t notices = count(both.log, "notifyUser: ");
	check(both.status == 0 && both.log.find("claimed hooking pass") != std::string::npos &&
	          notices == 1,
	      "without Steam's allocator exports load() stops with one notice (" +
	          std::to_string(notices) + ")" + status(both));

	// The policy is published before the setup lock lets a secondary
	// namespace skip, so its objects get the narrowed flags (none for libm).
	const Result secondary = run(root, "secondary");
	check(secondary.status == 0 &&
	          secondary.log.find("another auditor namespace already initialized") != std::string::npos,
	      "a namespace that skips setup still applies the narrowed audit policy (flags " +
	          std::to_string(secondary.status) + ")");

	std::filesystem::remove_all(root);
	return failures == 0 ? 0 : 1;
}
