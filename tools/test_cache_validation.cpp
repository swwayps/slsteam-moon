// readValidatedCacheBuffer reuses a validation only for the same pair: a pair
// replaced after it was validated is validated again. The startup Proton scan
// still reads the platform of each validated pair.
// Build: make test-cache-validation (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/log.hpp"

#include "base64/base64.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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
