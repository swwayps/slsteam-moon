// Directory walks reached from Steam's download hooks and the provisioning
// publish must not throw on a read error, and one odd entry (a dangling link)
// must skip only itself.
// Build (from repo root):
//   g++ -std=c++20 -Wall -Wextra -I src -I include tools/test_dir_walks.cpp -ldl -o /tmp/test_dir_walks && /tmp/test_dir_walks
#include "manifest_index.hpp"
#include "feats/synthmark.hpp"
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
namespace
{
int failures = 0;
void check(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
// Calls left before readdir fails with EIO; -1 never fails.
int g_readsBeforeError = -1;
template <typename Entry>
Entry* failingRead(const char* name, DIR* dir)
{
	using Fn = Entry* (*)(DIR*);
	static Fn real = nullptr;
	if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
	if (g_readsBeforeError == 0)
	{
		errno = EIO;
		return nullptr;
	}
	if (g_readsBeforeError > 0) --g_readsBeforeError;
	return real(dir);
}
}
extern "C" struct dirent* readdir(DIR* dir) { return failingRead<struct dirent>("readdir", dir); }
extern "C" struct dirent64* readdir64(DIR* dir) { return failingRead<struct dirent64>("readdir64", dir); }
int main()
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() /
		("slsteam_dir_walks_" + std::to_string(getpid()));
	fs::remove_all(dir);
	fs::create_directories(dir);
	std::set<uint64_t> expected;
	for (uint64_t gid = 1; gid <= 40; ++gid)
	{
		std::ofstream(dir / ("7_" + std::to_string(gid) + ".manifest")) << "m";
		std::ofstream(dir / ("synthetic_" + std::to_string(gid))) << "";
		expected.insert(gid);
	}
	fs::create_symlink(dir / "nowhere", dir / "7_999.manifest");

	std::set<uint64_t> seen;
	ManifestIndex::forEachManifest(dir.string(), [&](const ManifestIndex::ManifestName& name,
	                                                 fs::file_time_type) {
		seen.insert(name.gid);
	});
	check(seen == expected, "a dangling link skips only itself");

	g_readsBeforeError = 6;
	bool threw = false;
	try
	{
		ManifestIndex::forEachManifest(dir.string(),
		                               [](const ManifestIndex::ManifestName&, fs::file_time_type) {});
	}
	catch (...)
	{
		threw = true;
	}
	check(!threw, "a manifest walk stops on a read error instead of throwing");

	g_readsBeforeError = 6;
	threw = false;
	try
	{
		(void)SynthMark::loadAll(dir.string());
	}
	catch (...)
	{
		threw = true;
	}
	check(!threw, "the synthetic marker walk stops on a read error instead of throwing");
	g_readsBeforeError = -1;

	fs::remove_all(dir);
	return failures == 0 ? 0 : 1;
}
