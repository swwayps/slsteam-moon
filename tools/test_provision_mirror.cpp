// A cold-start pass walks the manifest store once: the terminal verdicts and
// the depot synthesis of every app reuse the index the pass was primed with.
// The provider mirror answers through tools/fake_libcurl.cpp.
// Build: make test-provision-mirror (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/feats/manifeststore.hpp"
#include "../src/log.hpp"
#include "fake_libcurl.hpp"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>

// Opens of the manifest store folder while armed. libstdc++ opens a folder
// through opendir, or through openat and fdopendir.
static std::atomic<bool> g_countOpens{false};
static std::atomic<int> g_storeOpens{0};
static std::string g_storeDir;

extern "C" DIR* opendir(const char* name)
{
	using Real = DIR* (*)(const char*);
	static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "opendir"));
	if (g_countOpens && name && std::string_view(name) == g_storeDir) ++g_storeOpens;
	return real(name);
}

extern "C" DIR* fdopendir(int fd)
{
	using Real = DIR* (*)(int);
	static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "fdopendir"));
	if (g_countOpens)
	{
		char link[64];
		std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
		char target[PATH_MAX];
		const ssize_t n = readlink(link, target, sizeof target);
		if (n > 0 && std::string_view(target, static_cast<std::size_t>(n)) == g_storeDir)
			++g_storeOpens;
	}
	return real(fd);
}

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

// The provider mirror. App 1001 lists one depot nobody holds a key for; every
// other app comes back without depots, as a token-locked one does.
class Provider final : public FakeCurl
{
public:
	std::atomic<int> requests{0};

	CURL* init() override { return reinterpret_cast<CURL*>(new Handle); }

	CURLcode setopt(CURL* c, CURLoption option, long, void* pointer) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		if (option == CURLOPT_URL) h.url = static_cast<const char*>(pointer);
		else if (option == CURLOPT_WRITEFUNCTION) h.write = reinterpret_cast<Write>(pointer);
		else if (option == CURLOPT_WRITEDATA) h.writeData = pointer;
		return CURLE_OK;
	}

	CURLcode perform(CURL* c) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		++requests;
		const std::string id = h.url.substr(h.url.rfind('/') + 1);
		const std::string depots = id == "1001"
			? ",\"depots\":{\"10012\":{\"manifests\":{\"public\":{\"gid\":\"888\"}}}}"
			: "";
		std::string body = "{\"data\":{\"" + id + "\":{\"appid\":\"" + id +
			"\",\"common\":{\"name\":\"App " + id + "\",\"type\":\"Game\"}" + depots +
			",\"_change_number\":5}},\"status\":\"success\"}";
		return h.write(body.data(), 1, body.size(), h.writeData) == body.size()
			? CURLE_OK : CURLE_WRITE_ERROR;
	}

	void cleanup(CURL* c) override { delete reinterpret_cast<Handle*>(c); }

	CURLcode getinfo(CURL*, CURLINFO info, void* out) override
	{
		if (info == CURLINFO_RESPONSE_CODE) *static_cast<long*>(out) = 200;
		return CURLE_OK;
	}

	CURLcode send(CURL*, const void*, std::size_t, std::size_t*) override
	{
		return CURLE_UNSUPPORTED_PROTOCOL;
	}

	CURLcode recv(CURL*, void*, std::size_t, std::size_t*) override
	{
		return CURLE_UNSUPPORTED_PROTOCOL;
	}

private:
	using Write = std::size_t (*)(char*, std::size_t, std::size_t, void*);
	struct Handle
	{
		std::string url;
		Write write = nullptr;
		void* writeData = nullptr;
	};
};

// An archived manifest that passes the store's validity check.
void archive(uint32_t depotId, uint64_t gid)
{
	std::ofstream out(g_storeDir + "/" + std::to_string(depotId) + "_" +
	                  std::to_string(gid) + ".manifest", std::ios::binary);
	const uint32_t words[4] = {0x71F617D0u, 0, 0, 0x32C415ABu};
	out.write(reinterpret_cast<const char*>(words), sizeof words);
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_provision_mirror_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	setenv("SLSSTEAM_DISABLE_CM", "1", 1);
	setenv("SLSSTEAM_APPINFO_PROVIDER", "https://provider.test/{appid}", 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	Provider provider;
	g_fakeCurl = &provider;

	// Twenty apps. 1000..1004 hold a managed depot key; the depots of 1000
	// and 1001 also have an archived manifest.
	std::unordered_set<uint32_t> apps;
	for (uint32_t appId = 1000; appId < 1020; ++appId) apps.insert(appId);
	for (uint32_t appId = 1000; appId < 1005; ++appId)
		DepotKey::saveKeyToCache(appId, appId * 10 + 1, std::string(32, 'k'), true);
	g_storeDir = ManifestStore::dir();
	std::filesystem::create_directories(g_storeDir);
	archive(10001, 555);
	archive(10011, 777);
	g_config.managedAppIds.set(apps);

	g_countOpens = true;
	AppInfoProvision::provisionColdStartApps(apps);
	g_countOpens = false;
	const int walks = g_storeOpens.load();
	CHECK(provider.requests == 20, "the provider is asked once for each app");
	CHECK(walks == 1,
	      ("the pass walks the manifest store once (" + std::to_string(walks) +
	       " walks for 20 apps)").c_str());
	const std::string cache = std::string(home) + "/SLSsteam/cache";
	CHECK(std::filesystem::exists(cache + "/picsbuffer_1000.bin"),
	      "a depot-less app is rebuilt from its archived manifest");
	// 1001's verdict depends on its archived manifest: the check below fails
	// when the verdict is fingerprinted against another index.
	CHECK(std::filesystem::exists(cache + "/terminal_1001.state") &&
	      !std::filesystem::exists(cache + "/picsbuffer_1001.bin"),
	      "an app whose only listed depot has no key gets a verdict");

	// The verdicts were recorded against the store as it is, so the next
	// start skips every app.
	AppInfoProvision::provisionColdStartApps(apps);
	CHECK(provider.requests == 20,
	      ("the next start reuses every verdict (" +
	       std::to_string(provider.requests.load() - 20) + " apps asked again)").c_str());

	if (g_failures == 0) std::printf("\nall provision mirror checks passed\n");
	else std::printf("\n%d provision mirror check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
