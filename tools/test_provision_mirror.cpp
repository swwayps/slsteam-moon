// A cold-start pass walks the manifest store once: the terminal verdicts and
// the depot synthesis of every app reuse the index the pass was primed with.
// It flushes its cache writes once, at its end.
// A mirror answer is capped at 16 MiB and its redirects stay on https. A
// force-full changes answer refreshes the locally authoritative apps, and a
// refresh that renders the pair on disk does not write it again.
// The provider mirror answers through tools/fake_libcurl.cpp.
// Build: make test-provision-mirror (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/appinfostate.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/feats/manifeststore.hpp"
#include "../src/feats/pics.hpp"
#include "../src/log.hpp"
#include "../src/sdk/protobufs/steammessages_clientserver_appinfo.pb.h"
#include "fake_libcurl.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
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

// fsync and syncfs calls while armed.
static std::atomic<bool> g_countSyncs{false};
static std::atomic<int> g_fsyncs{0};
static std::atomic<int> g_syncfs{0};

extern "C" int fsync(int fd)
{
	using Real = int (*)(int);
	static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "fsync"));
	if (g_countSyncs) ++g_fsyncs;
	return real(fd);
}

extern "C" int syncfs(int fd)
{
	using Real = int (*)(int);
	static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "syncfs"));
	if (g_countSyncs) ++g_syncfs;
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

// The provider mirror. App 1001 lists one depot nobody holds a key for; app
// 2000 answers with 17 MiB; every other app comes back without depots, as a
// token-locked one does.
class Provider final : public FakeCurl
{
public:
	std::atomic<int> requests{0};
	std::atomic<int> changeNumber{5}; // in every app's answer
	std::size_t bigAccepted = 0; // most bytes of 2000's answer one GET kept
	bool redirectsHttps = true; // every GET limited its redirects to https

	CURL* init() override { return reinterpret_cast<CURL*>(new Handle); }

	CURLcode setopt(CURL* c, CURLoption option, long number, void* pointer) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		if (option == CURLOPT_URL) h.url = static_cast<const char*>(pointer);
		else if (option == CURLOPT_WRITEFUNCTION) h.write = reinterpret_cast<Write>(pointer);
		else if (option == CURLOPT_WRITEDATA) h.writeData = pointer;
		else if (option == CURLOPT_REDIR_PROTOCOLS) h.redirProtocols = number;
		return CURLE_OK;
	}

	CURLcode perform(CURL* c) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		std::string id;
		std::string body;
		if (h.url.rfind("https://api.steampowered.com/ISteamDirectory/", 0) == 0)
		{
			// The CM directory lists no server: a CM batch fails without a
			// connectivity verdict, so the mirror is tried. CM sockets are
			// unreachable.
			body = "{\"response\":{\"serverlist\":[]}}";
		}
		else if (h.url.rfind("https://provider.test/", 0) != 0 &&
		         h.url.rfind("https://api.steamcmd.net/", 0) != 0)
			return CURLE_COULDNT_RESOLVE_HOST;
		else
		{
			++requests;
			if (h.redirProtocols != CURLPROTO_HTTPS) redirectsHttps = false;
			id = h.url.substr(h.url.rfind('/') + 1);
			const std::string depots = id == "1001"
				? ",\"depots\":{\"10012\":{\"manifests\":{\"public\":{\"gid\":\"888\"}}}}"
				: "";
			const std::string pad = id == "2000"
				? ",\"pad\":\"" + std::string(17u << 20, 'x') + "\"" : "";
			body = "{\"data\":{\"" + id + "\":{\"appid\":\"" + id +
				"\",\"common\":{\"name\":\"App " + id + "\",\"type\":\"Game\"" + pad + "}" +
				depots + ",\"_change_number\":" + std::to_string(changeNumber.load()) +
				"}},\"status\":\"success\"}";
		}
		// Delivered in chunks, like libcurl: a short count ends the transfer.
		std::size_t accepted = 0;
		for (std::size_t at = 0; at < body.size(); at += 65536)
		{
			const std::size_t chunk = std::min<std::size_t>(65536, body.size() - at);
			const std::size_t kept =
				h.write(const_cast<char*>(body.data()) + at, 1, chunk, h.writeData);
			accepted += kept;
			if (kept != chunk) break;
		}
		if (id == "2000") bigAccepted = std::max(bigAccepted, accepted);
		return accepted == body.size() ? CURLE_OK : CURLE_WRITE_ERROR;
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
		long redirProtocols = -1;
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
	g_countSyncs = true;
	AppInfoProvision::provisionColdStartApps(apps);
	g_countSyncs = false;
	g_countOpens = false;
	const int walks = g_storeOpens.load();
	CHECK(provider.requests == 20, "the provider is asked once for each app");
	CHECK(walks == 1,
	      ("the pass walks the manifest store once (" + std::to_string(walks) +
	       " walks for 20 apps)").c_str());
	// Each fsync costs a journal commit (about 20 ms on a virtual disk), and
	// this pass runs before Steam shows anything.
	CHECK(g_fsyncs == 0 && g_syncfs == 1,
	      ("the pass flushes its cache writes once, at its end (" +
	       std::to_string(g_fsyncs.load()) + " fsync, " +
	       std::to_string(g_syncfs.load()) + " syncfs for 20 apps)").c_str());
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

	// An answer larger than a cache pair (16 MiB) is refused while it arrives,
	// so nothing is parsed: the app gets neither a pair nor a verdict.
	apps.insert(2000);
	g_config.managedAppIds.set(apps);
	AppInfoProvision::provisionColdStartApps({2000});
	CHECK(provider.bigAccepted > 0 && provider.bigAccepted <= (16u << 20) &&
	      !std::filesystem::exists(cache + "/terminal_2000.state") &&
	      !std::filesystem::exists(cache + "/picsbuffer_2000.bin"),
	      ("an answer over 16 MiB is cut off and parsed nowhere (" +
	       std::to_string(provider.bigAccepted) + " bytes kept)").c_str());
	CHECK(provider.redirectsHttps,
	      "every mirror GET follows redirects to https only");

	// A force-full changes answer lists no apps, and Steam's own full refresh
	// leaves out locally authoritative ones, so each managed one is refreshed
	// here.
	// When the refreshed answer renders the pair already on disk, the pair is
	// not written again and the base's DLC metadata stays.
	setenv("SLSSTEAM_ASYNC_PROVISION", "1", 1);
	AppInfoState::publishAuthoritative({1000});
	const std::string pairPath = cache + "/picsbuffer_1000.bin";
	const std::string sidecarPath = cache + "/dlcmetadata_1000.yaml";
	std::ofstream(sidecarPath) << "kept\n";
	const auto occurrences = [&](const std::string& needle) {
		std::ifstream log(logPath);
		const std::string text(std::istreambuf_iterator<char>(log), {});
		int count = 0;
		for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
			++count;
		return count;
	};
	// Sends a force-full changes answer and waits for the refresh pass it starts.
	const auto forceFull = [&](uint32_t changeNumber, int pass) {
		CMsgClientPICSChangesSinceResponse changes;
		changes.set_current_change_number(changeNumber);
		changes.set_force_full_update(true);
		changes.set_force_full_app_update(true);
		PICS::recvChangesSinceResponse(&changes);
		for (int i = 0; i < 1000; ++i)
		{
			if (occurrences("pass origin=targeted requested=1 ") >= pass) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	};
	struct stat pairBefore{};
	stat(pairPath.c_str(), &pairBefore);
	int requestsBefore = provider.requests.load();
	const bool finished = forceFull(9000, 1);
	CHECK(occurrences("plan origin=force-full candidates=1 ") == 1,
	      "a force-full changes answer refreshes the authoritative managed app");
	struct stat pairAfter{};
	stat(pairPath.c_str(), &pairAfter);
	CHECK(finished && provider.requests.load() == requestsBefore + 1 &&
	          pairAfter.st_ino == pairBefore.st_ino &&
	          pairAfter.st_mtim.tv_sec == pairBefore.st_mtim.tv_sec &&
	          pairAfter.st_mtim.tv_nsec == pairBefore.st_mtim.tv_nsec &&
	          std::filesystem::exists(sidecarPath),
	      ("a refresh that renders the same pair leaves it and its DLC metadata alone (" +
	       std::to_string(provider.requests.load() - requestsBefore) + " fetch)").c_str());
	// The app changed upstream: the new pair replaces the old one, and the
	// DLC metadata of the old base goes.
	// (The first pass republished the authoritative set from this test's
	// config, which lists no added app.)
	provider.changeNumber = 6;
	AppInfoState::publishAuthoritative({1000});
	requestsBefore = provider.requests.load();
	const bool refreshed = forceFull(9001, 2);
	stat(pairPath.c_str(), &pairAfter);
	CHECK(refreshed && provider.requests.load() == requestsBefore + 1 &&
	          pairAfter.st_ino != pairBefore.st_ino && !std::filesystem::exists(sidecarPath),
	      ("a refresh that renders a changed pair writes it and drops the old DLC metadata (" +
	       std::string(refreshed ? "" : "no pass, ") +
	       std::to_string(provider.requests.load() - requestsBefore) + " fetch, pair " +
	       (pairAfter.st_ino != pairBefore.st_ino ? "written" : "kept") + ", metadata " +
	       (std::filesystem::exists(sidecarPath) ? "kept" : "dropped") + ")").c_str());

	if (g_failures == 0) std::printf("\nall provision mirror checks passed\n");
	else std::printf("\n%d provision mirror check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
