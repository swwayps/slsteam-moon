// A local pattern cache that lacks an optional locator the embedded resolver
// then finds (a later build added the locator, or fixed its signature) is
// written again with it, so the next boot reads it instead of scanning again.
// A boot served by a published catalog writes the local cache too, so later
// boots do not hash the whole module to find that catalog again.
// Build: make test-local-catalog (links the whole object set).
#include "../src/globals.hpp"
#include "../src/log.hpp"
#include "../src/patterns.hpp"
#include "../src/utils.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>

extern "C" char __executable_start;
extern "C" char etext;

// Two byte runs in this binary's code that no compiled signature matches.
// They are never executed.
asm(".pushsection .text\n"
    ".byte 0x8f,0x3a,0xe1,0x77,0xc2,0x19,0xb4,0x5d,0x06,0xee,0x2b,0x91,0x7c,0xd3,0x48,0xa0\n"
    ".byte 0x5b,0xc8,0x1e,0x92,0x3f,0xa7,0x60,0xd4,0x0b,0x7e,0xe9,0x25,0x86,0x4c,0xf1,0x3d\n"
    ".popsection\n");

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

Pattern_t g_kept {"LocalCacheKept",
	"8F 3A E1 77 C2 19 B4 5D 06 EE 2B 91 7C D3 48 A0", MemHlp::SigFollowMode::None};
Pattern_t g_added {"LocalCacheAdded",
	"5B C8 1E 92 3F A7 60 D4 0B 7E E9 25 86 4C F1 3D", MemHlp::SigFollowMode::None};

std::filesystem::path cacheFile(const std::filesystem::path& home)
{
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(
		home / "SLSsteam" / "patterns" / "local" / "steamclient", error))
	{
		if (entry.path().extension() == ".cache")
			return entry.path();
	}
	return {};
}

std::string slurp(const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream body;
	body << in.rdbuf();
	return body.str();
}

ino_t inode(const std::filesystem::path& path)
{
	struct stat value {};
	return stat(path.c_str(), &value) == 0 ? value.st_ino : 0;
}

bool has(const std::string& body, const char* text)
{
	return body.find(text) != std::string::npos;
}

// What the log gained since `offset` bytes.
std::string logSince(const std::string& path, std::size_t offset)
{
	const std::string body = slurp(path);
	return offset < body.size() ? body.substr(offset) : std::string();
}

// A published catalog for this binary naming where the two fixture locators
// resolved, in the canonical form the pattern-refresh helper activates.
std::string publishedCatalog(const std::string& path)
{
	std::string body = "schema = 1\nplatform = \"linux32\"\ncomponent = \"steamclient\"\n"
	                   "module_name = \"steamclient.so\"\nmodule_sha256 = \"" +
	                   Utils::getFileSHA256(path.c_str()) + "\"\nmodule_size = " +
	                   std::to_string(std::filesystem::file_size(path)) + "\ngnu_build_id = \"" +
	                   Utils::getBuildId(path.c_str()) + "\"\nsteam_version = 1\n"
	                   "consumer_commit = \"" + std::string(40, 'a') + "\"\n"
	                   "minimum_consumer_schema = 1\nrevision = 1\nsource = \"deterministic\"\n";
	for (const Pattern_t* pattern : {&g_kept, &g_added})
	{
		const std::string rva = std::to_string(pattern->matchAddress - g_modSteamClient.base);
		body += "\n[[locators]]\nsymbol = \"" + pattern->symbol + "\"\nname = \"" +
		        pattern->name + "\"\ntarget_rva = " + rva + "\nmatch_rva = " + rva +
		        "\nsignature = \"" + pattern->pattern + "\"\nfollow_mode = \"None\"\n"
		        "resolver = \"signature\"\nrequired = false\nmatch_count = 1\ntarget_count = 1\n";
	}
	return body;
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_local_catalog_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	setenv("SLSSTEAM_PATTERN_CACHE", "1", 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	// This binary stands in for steamclient.so.
	const std::string self = std::filesystem::read_symlink("/proc/self/exe");
	g_modSteamClient.base = reinterpret_cast<lm_address_t>(&__executable_start);
	g_modSteamClient.end = reinterpret_cast<lm_address_t>(&etext);
	g_modSteamClient.size = g_modSteamClient.end - g_modSteamClient.base;
	std::strncpy(g_modSteamClient.path, self.c_str(), sizeof(g_modSteamClient.path) - 1);

	// No compiled signature matches this binary: each one degrades instead of
	// failing the pass, and only the two locators above resolve.
	auto& registry = Patterns::patterns();
	for (Pattern_t* pattern : registry)
		pattern->optional = true;

	// A build without the added locator writes the first cache.
	registry.erase(std::remove(registry.begin(), registry.end(), &g_added), registry.end());
	CHECK(Patterns::init(), "the older build's locator pass succeeds");
	const auto file = cacheFile(home);
	CHECK(!file.empty() && has(slurp(file), "locator_count=1\n"),
	      "it writes a cache holding its one resolved locator");

	// The next build loads that cache and has to scan for the added locator.
	registry.push_back(&g_added);
	CHECK(Patterns::init(), "the newer build's locator pass succeeds");
	CHECK(g_added.address != LM_ADDRESS_BAD, "the embedded resolver finds the added locator");
	const std::string rewritten = slurp(file);
	CHECK(has(rewritten, "symbol=Patterns::LocalCacheAdded\n") && has(rewritten, "locator_count=2\n"),
	      "the cache is written again with the added locator");

	// Every other locator misses again on the next boot; misses alone do not
	// rewrite the cache.
	const ino_t written = inode(file);
	CHECK(Patterns::init(), "a third locator pass succeeds");
	CHECK(written != 0 && inode(file) == written && slurp(file) == rewritten,
	      "a pass that resolves nothing new leaves the cache alone");
	CHECK(g_added.address != LM_ADDRESS_BAD, "the added locator still resolves");

	// A boot served by a published catalog keeps its result as the local cache,
	// so later boots skip the published path and its hash of the whole module.
	char published[] = "/tmp/test_local_catalog_pub_XXXXXX";
	if (!mkdtemp(published)) return 2;
	setenv("HOME", published, 1);
	setenv("XDG_CONFIG_HOME", published, 1);
	const auto catalogDir = std::filesystem::path(published) / "SLSsteam" / "patterns" / "steamclient";
	std::filesystem::create_directories(catalogDir);
	std::ofstream(catalogDir / (Utils::getFileSHA256(self.c_str()) + ".toml"), std::ios::binary)
		<< publishedCatalog(self);
	std::size_t mark = slurp(logPath).size();
	CHECK(Patterns::init(), "a pass served by the published catalog succeeds");
	CHECK(has(logSince(logPath, mark), "Pattern catalog: loaded steamclient revision 1"),
	      "that pass loads the published catalog");
	const auto local = cacheFile(published);
	CHECK(!local.empty() && has(slurp(local), "locator_count=2\n"),
	      "it keeps the published result as the local cache");
	mark = slurp(logPath).size();
	CHECK(Patterns::init(), "the next pass succeeds");
	std::string pass = logSince(logPath, mark);
	CHECK(has(pass, "Pattern cache: loaded steamclient local catalog")
	      && !has(pass, "Pattern catalog: loaded steamclient"),
	      "the next pass reads the local cache instead of the published catalog");

	// A local cache from an older locator policy is replaced as well, instead
	// of being rejected again on every later boot.
	if (!local.empty())
	{
		std::string stale = slurp(local);
		const auto at = stale.find("signature=8F 3A");
		if (at != std::string::npos) stale.replace(at, 15, "signature=8F 3B");
		std::ofstream(local, std::ios::binary | std::ios::trunc) << stale;
	}
	mark = slurp(logPath).size();
	CHECK(Patterns::init(), "a pass with a stale local cache succeeds");
	pass = logSince(logPath, mark);
	CHECK(has(pass, "changed compiled locator policy")
	      && has(pass, "Pattern catalog: loaded steamclient revision 1"),
	      "it rejects the stale cache and uses the published catalog");
	mark = slurp(logPath).size();
	CHECK(Patterns::init(), "the pass after it succeeds");
	pass = logSince(logPath, mark);
	CHECK(!has(pass, "changed compiled locator policy")
	      && has(pass, "Pattern cache: loaded steamclient local catalog"),
	      "the stale cache was replaced, so the next pass loads it without a warning");

	if (g_failures == 0) std::printf("\nall local catalog checks passed\n");
	else std::printf("\n%d local catalog check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
