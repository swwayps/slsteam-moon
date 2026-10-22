// A published locator catalog names a target only while the bytes it matched
// still match the compiled signature; otherwise the embedded resolver runs for
// that locator and every later one.
// Build: make test-catalog-trust (links the whole object set).
#include "../src/globals.hpp"
#include "../src/log.hpp"
#include "../src/pattern_catalog.hpp"
#include "../src/patterns.hpp"
#include "../src/utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>

extern "C" char __executable_start;
extern "C" char etext;

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

// Executable bytes that match no compiled signature.
__attribute__((noinline)) int anchor(int value)
{
	return value * 3 + 1;
}

const char* followModeName(MemHlp::SigFollowMode mode)
{
	switch (mode)
	{
		case MemHlp::SigFollowMode::Relative: return "Relative";
		case MemHlp::SigFollowMode::PrologueUpwards: return "PrologueUpwards";
		default: return "None";
	}
}

// A catalog for this binary in which every steamclient locator, matched or
// targeted, points at `anchor`.
std::string catalogFor(const std::string& path, std::uint64_t rva)
{
	const auto size = std::filesystem::file_size(path);
	std::string body = "schema = 1\nplatform = \"linux32\"\ncomponent = \"steamclient\"\n"
	                   "module_name = \"steamclient.so\"\nmodule_sha256 = \"" +
	                   Utils::getFileSHA256(path.c_str()) + "\"\nmodule_size = " +
	                   std::to_string(size) + "\ngnu_build_id = \"" +
	                   Utils::getBuildId(path.c_str()) + "\"\nsteam_version = 1\n"
	                   "consumer_commit = \"" + std::string(40, 'a') + "\"\n"
	                   "minimum_consumer_schema = 1\nrevision = 1\nsource = \"deterministic\"\n";
	for (const Pattern_t* pattern : Patterns::patterns())
	{
		if (pattern->module == &g_modSteamUI) continue;
		body += "\n[[locators]]\nsymbol = \"" + pattern->symbol + "\"\nname = \"" +
		        pattern->name + "\"\ntarget_rva = " + std::to_string(rva) +
		        "\nmatch_rva = " + std::to_string(rva) + "\nsignature = \"" +
		        pattern->pattern + "\"\nfollow_mode = \"" + followModeName(pattern->followMode) +
		        "\"\nresolver = \"signature\"\nrequired = " +
		        (pattern->optional ? "false" : "true") + "\nmatch_count = 1\ntarget_count = 1\n";
	}
	return body;
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_catalog_trust_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	// This binary stands in for steamclient.so.
	const std::string self = std::filesystem::read_symlink("/proc/self/exe");
	g_modSteamClient.base = reinterpret_cast<lm_address_t>(&__executable_start);
	g_modSteamClient.end = reinterpret_cast<lm_address_t>(&etext);
	g_modSteamClient.size = g_modSteamClient.end - g_modSteamClient.base;
	std::strncpy(g_modSteamClient.path, self.c_str(), sizeof(g_modSteamClient.path) - 1);

	// The first pass applies the compiled optional flags the catalog must repeat.
	(void)Patterns::init();
	const std::uint64_t rva =
		reinterpret_cast<lm_address_t>(&anchor) - g_modSteamClient.base;
	const auto dir = std::filesystem::path(home) / "SLSsteam" / "patterns" / "steamclient";
	std::filesystem::create_directories(dir);
	std::ofstream(dir / (Utils::getFileSHA256(self.c_str()) + ".toml"), std::ios::binary)
		<< catalogFor(self, rva);

	(void)Patterns::init();
	const lm_address_t named = g_modSteamClient.base + static_cast<lm_address_t>(rva);
	bool used = false;
	for (const Pattern_t* pattern : Patterns::patterns())
		used = used || pattern->address == named;
	CHECK(!used, "a catalog target whose bytes no longer match the signature is not used");
	std::ifstream logFile(logPath);
	std::stringstream log;
	log << logFile.rdbuf();
	std::size_t rejected = 0;
	for (auto at = log.str().find("Pattern catalog entry for"); at != std::string::npos;
	     at = log.str().find("Pattern catalog entry for", at + 1))
		++rejected;
	CHECK(rejected == 1, "the first rejected entry drops the rest of the catalog");
	CHECK(anchor(1) == 4, "the stand-in code still runs");

	if (g_failures == 0) std::printf("\nall catalog trust checks passed\n");
	else std::printf("\n%d catalog trust check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
