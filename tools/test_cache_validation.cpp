// readValidatedCacheBuffer reuses a validation only for the same pair: a pair
// replaced after it was validated is validated again. The startup Proton scan
// still reads the platform of each validated pair.
// Build: make test-cache-validation (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/log.hpp"
#include "../src/utils/process_lock.hpp"

#include "base64/base64.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <thread>

// Live heap allocations, to see what a code path keeps.
static std::atomic<long> g_liveAllocations{0};
void* operator new(std::size_t size)
{
	if (void* p = std::malloc(size ? size : 1))
	{
		++g_liveAllocations;
		return p;
	}
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept
{
	if (!p) return;
	--g_liveAllocations;
	std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

constexpr uint32_t kApp = 42;

std::string wireWithGid(const char* gid, const char* oslist = "linux")
{
	return std::string("\"appinfo\"\n{\n\t\"depots\"\n\t{\n\t\t\"43\"\n\t\t{\n"
	                   "\t\t\t\"config\"\n\t\t\t{\n\t\t\t\t\"oslist\"\t\"") +
	       oslist +
	       "\"\n\t\t\t}\n\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
	       "\t\t\t\t\t\"gid\"\t\"" +
	       gid + "\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
}

std::string metadataFor(const std::string& wire, uint32_t appId = kApp)
{
	std::uint8_t sha[20]{};
	AppInfoProvision::sha1Bytes(wire.data(), wire.size(), sha);
	return "appid: " + std::to_string(appId) + "\nchange_number: 7\nwire_size: " +
	       std::to_string(wire.size()) +
	       "\nsha_b64: " + base64::to_base64(std::string(reinterpret_cast<char*>(sha), 20)) +
	       "\nnormalized: true\nsynthetic: false\n";
}

// Replaces the file with a new one, as every cache writer does.
void replaceFile(const std::string& path, const std::string& text)
{
	std::ofstream(path + ".tmp", std::ios::binary) << text;
	std::filesystem::rename(path + ".tmp", path);
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_cache_validation_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	const std::string cache = std::string(home) + "/SLSsteam/cache";
	std::filesystem::create_directories(cache);
	const std::string buffer = cache + "/picsbuffer_42.bin";
	const std::string metadata = cache + "/picsbuffer_42.yaml";

	const std::string wire = wireWithGid("1234567890");
	replaceFile(buffer, wire);
	replaceFile(metadata, metadataFor(wire));
	std::string read;
	CHECK(AppInfoProvision::readValidatedCacheBuffer(kApp, read) && read == wire,
	      "a valid pair is read");
	read.clear();
	CHECK(AppInfoProvision::readValidatedCacheBuffer(kApp, read) && read == wire,
	      "an unchanged pair is read again");

	// Same size, other bytes, metadata left as it was: the SHA-1 no longer matches.
	replaceFile(buffer, wireWithGid("1234567891"));
	CHECK(!AppInfoProvision::readValidatedCacheBuffer(kApp, read),
	      "a pair replaced after its validation is validated again");

	const std::string empty = wireWithGid("0");
	replaceFile(buffer, empty);
	replaceFile(metadata, metadataFor(empty));
	CHECK(!AppInfoProvision::readValidatedCacheBuffer(kApp, read),
	      "a pair without a usable depot is rejected");

	// Every refresh replaces the pair; the memo keeps one validation per app.
	{
		const auto cycle = [&](int i)
		{
			const std::string next = wireWithGid(std::to_string(1000000000 + i).c_str());
			replaceFile(buffer, next);
			replaceFile(metadata, metadataFor(next));
			return AppInfoProvision::readValidatedCacheBuffer(kApp, read);
		};
		bool allRead = cycle(0) && cycle(1);
		const long before = g_liveAllocations.load();
		for (int i = 2; i < 202; ++i) allRead = cycle(i) && allRead;
		const long grown = g_liveAllocations.load() - before;
		CHECK(allRead && grown < 20,
		      ("replaced pairs do not pile up in the validation memo (+" +
		       std::to_string(grown) + " live allocations)").c_str());
	}

	// Planner and manifest hooks classify the same depots over and over; a
	// repeat for an unchanged pair answers without the cache lock or a read.
	{
		const uint32_t base = 46;
		const auto taggedWire = [](const char* dlc)
		{
			return std::string("\"appinfo\"\n{\n\t\"depots\"\n\t{\n"
			                   "\t\t\"4601\"\n\t\t{\n\t\t\t\"manifests\"\n\t\t\t{\n"
			                   "\t\t\t\t\"public\"\n\t\t\t\t{\n\t\t\t\t\t\"gid\"\t\"11\"\n"
			                   "\t\t\t\t}\n\t\t\t}\n\t\t}\n"
			                   "\t\t\"4602\"\n\t\t{\n\t\t\t\"dlcappid\"\t\"") +
			       dlc + "\"\n\t\t}\n\t}\n}\n";
		};
		const std::string tagged = taggedWire("47");
		replaceFile(cache + "/picsbuffer_46.bin", tagged);
		replaceFile(cache + "/picsbuffer_46.yaml", metadataFor(tagged, base));
		const bool classified =
			AppInfoProvision::provisionedDlcAppIdForDepot(base, 4602) == 47 &&
			AppInfoProvision::provisionedDlcAppIdForDepot(base, 4601) == 0;
		auto answer = std::make_shared<std::atomic<long>>(-1);
		{
			ProcessLock::FileLock holder(AppInfoProvision::cacheLockPath(), false);
			std::thread([answer, base] {
				answer->store(AppInfoProvision::provisionedDlcAppIdForDepot(base, 4602));
			}).detach();
			for (int i = 0; i < 100 && answer->load() < 0; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			CHECK(classified && answer->load() == 47,
			      "a repeated depot query does not wait for the cache lock");
		}
		const std::string retagged = taggedWire("48");
		replaceFile(cache + "/picsbuffer_46.bin", retagged);
		replaceFile(cache + "/picsbuffer_46.yaml", metadataFor(retagged, base));
		CHECK(AppInfoProvision::provisionedDlcAppIdForDepot(base, 4602) == 48,
		      "a republished pair is classified again");
	}

	// 42 ships only a Windows depot, 44 a Linux one.
	const std::string windows = wireWithGid("1234567890", "windows");
	replaceFile(buffer, windows);
	replaceFile(metadata, metadataFor(windows));
	const std::string native = wireWithGid("1234567890", "linux");
	replaceFile(cache + "/picsbuffer_44.bin", native);
	replaceFile(cache + "/picsbuffer_44.yaml", metadataFor(native, 44));
	g_config.managedAppIds.set({kApp, 44});
	AppInfoProvision::flushPendingProtonMappings();
	CHECK(AppInfoProvision::requiresProton(kApp) && !AppInfoProvision::requiresProton(44),
	      "the startup scan marks only the Windows-only pair for Proton");

	// The preinit Proton mapping edit treats an unusable Steam config folder
	// (a symlink loop here) as absent instead of throwing out of setup().
	const std::string steam = std::string(home) + "/.local/share/Steam";
	std::filesystem::create_directories(steam);
	std::ofstream(steam + "/steam.sh") << "#!/bin/sh\n";
	std::filesystem::create_directory_symlink("config", steam + "/config");
	bool flushThrew = false;
	try { AppInfoProvision::flushPendingProtonMappings(); }
	catch (...) { flushThrew = true; }
	CHECK(!flushThrew, "an unusable Steam config folder does not throw out of the Proton mapping edit");

	if (g_failures == 0) std::printf("\nall cache validation checks passed\n");
	else std::printf("\n%d cache validation check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
