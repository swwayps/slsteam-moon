// A manifest blob job that fails to start (an allocation failure) leaves no
// entry behind: the next wait for that manifest starts a new job instead of
// getting the broken promise of one that never ran.
// Build: make test-blob-alloc (links the whole object set).
#include "../src/log.hpp"
#include "../src/utils/ManifestFetch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

// Counts down allocations on this thread; the one that reaches zero fails.
static thread_local int t_failIn = 0;
static thread_local bool t_failed = false;

void* operator new(std::size_t size)
{
	if (t_failIn > 0 && --t_failIn == 0)
	{
		t_failed = true;
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
	char home[] = "/tmp/test_blob_alloc_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((std::string(home) + "/test.log").c_str()));
	const std::string steam = std::string(home) + "/.local/share/Steam";
	std::filesystem::create_directories(steam + "/depotcache");
	std::ofstream(steam + "/steam.sh") << "#!/bin/sh\n";

	int rounds = 0;
	int poisoned = 0;
	for (int k = 1; k <= 200; ++k)
	{
		const uint64_t gid = 7000 + static_cast<uint64_t>(k);
		t_failed = false;
		t_failIn = k;
		try { (void)ManifestFetch::awaitManifestBlobFor(gid, 4201, 0, false, false); }
		catch (...) {}
		t_failIn = 0;
		if (!t_failed) break; // the call no longer reaches its k-th allocation
		++rounds;
		bool threw = false;
		try { (void)ManifestFetch::awaitManifestBlobFor(gid, 4201, 0, false, false); }
		catch (...) { threw = true; }
		if (threw && poisoned == 0) poisoned = k;
	}
	CHECK(rounds > 0, "the start of a blob job was failed at every allocation");
	if (poisoned)
		std::printf("first poisoned allocation: %d of %d\n", poisoned, rounds);
	CHECK(poisoned == 0,
	      "a blob job that failed to start leaves nothing a later wait trips over");

	if (g_failures == 0) std::printf("\nall blob allocation checks passed\n");
	else std::printf("\n%d blob allocation check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
