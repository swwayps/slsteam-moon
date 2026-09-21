
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../feats/manifeststore.hpp"
#include "../log.hpp"
#include "../cainfo.hpp"
#include "boundedexecutor.hpp"
#include "../sdk/IClientAppManager.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace ManifestFetch
{

namespace
{

std::atomic<bool> g_providersOffline{false};
std::atomic<uint64_t> g_sessionEpoch{1};

void setOfflineStatus(bool offline)
{
	const char* home = std::getenv("HOME");
	if (!home) return;
	std::string path = std::string(home) + "/.config/SLSsteam/offline";
	if (offline)
	{
		std::ofstream f(path);
		if (f.is_open())
		{
			f << "1\n";
		}
	}
	else
	{
		std::remove(path.c_str());
	}
}

struct OfflineCleaner
{
	OfflineCleaner()
	{
		const char* home = std::getenv("HOME");
		if (home)
		{
			std::string path = std::string(home) + "/.config/SLSsteam/offline";
			std::remove(path.c_str());
			std::string dir = std::string(home) + "/.config/SLSsteam";
			if (std::filesystem::exists(dir))
			{
				std::error_code ec;
				for (auto& entry : std::filesystem::directory_iterator(dir, ec))
				{
					if (entry.is_regular_file() && entry.path().filename().string().rfind("offline_", 0) == 0)
					{
						std::filesystem::remove(entry.path(), ec);
					}
				}
			}
		}
	}
};
static OfflineCleaner g_offlineCleaner;

std::mutex g_circuitLock;
ProviderCircuit g_providerCircuit(
	/*failureThreshold=*/2, /*cooldownMs=*/30000);

std::int64_t steadyNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t beginProviderAttempt(uint64_t sessionEpoch)
{
	std::lock_guard<std::mutex> lock(g_circuitLock);
	if (sessionEpoch != g_sessionEpoch.load(std::memory_order_acquire))
		return 0;
	return g_providerCircuit.beginAttempt(steadyNowMs());
}

void finishProviderAttempt(uint64_t sessionEpoch,
	                         uint64_t attemptToken,
	                         bool success, bool rateLimited,
	                         bool transportFailure)
{
	bool wasOffline = false;
	bool nowOffline = false;
	{
		std::lock_guard<std::mutex> lock(g_circuitLock);
		if (sessionEpoch != g_sessionEpoch.load(std::memory_order_acquire))
		{
			g_providerCircuit.cancelAttempt(attemptToken);
			return;
		}
		wasOffline = g_providerCircuit.open();
		g_providerCircuit.finishAttempt(
		    attemptToken, steadyNowMs(), success, rateLimited, transportFailure);
		nowOffline = g_providerCircuit.open();
	}
	g_providersOffline.store(nowOffline, std::memory_order_release);
	setOfflineStatus(nowOffline);
	if (!wasOffline && nowOffline)
	{
		g_pLog->info(
		    "ManifestFetch: manifest archive unreachable; marking providers offline until it recovers\n");
	}
	else if (wasOffline && !nowOffline)
	{
		g_pLog->info(
		    "ManifestFetch: manifest archive reachable again; clearing offline state\n");
	}
}

std::mutex g_missingNotifyLock;
std::set<std::pair<uint32_t, uint64_t>> g_missingNotified;

bool markMissingForNotification(uint32_t depotId, uint64_t gid,
	                            uint64_t epoch)
{
	if (epoch != g_sessionEpoch.load(std::memory_order_acquire)) return false;
	std::lock_guard lock(g_missingNotifyLock);
	if (epoch != g_sessionEpoch.load(std::memory_order_acquire)) return false;
	return g_missingNotified.emplace(depotId, gid).second;
}

constexpr auto kArchiveMissTtl = std::chrono::minutes(10);
std::mutex g_archiveMissLock;
std::map<std::pair<uint32_t, uint64_t>, std::chrono::steady_clock::time_point>
	    g_archiveMisses;

bool hasRecentArchiveMiss(uint32_t depotId, uint64_t gid)
{
	std::lock_guard lock(g_archiveMissLock);
	const auto key = std::make_pair(depotId, gid);
	const auto it = g_archiveMisses.find(key);
	if (it == g_archiveMisses.end()) return false;
	if (std::chrono::steady_clock::now() - it->second < kArchiveMissTtl)
		return true;
	g_archiveMisses.erase(it);
	return false;
}

void rememberArchiveMiss(uint32_t depotId, uint64_t gid)
{
	std::lock_guard lock(g_archiveMissLock);
	if (g_archiveMisses.size() >= 100000) g_archiveMisses.clear();
	g_archiveMisses[{depotId, gid}] = std::chrono::steady_clock::now();
}

void clearArchiveMiss(uint32_t depotId, uint64_t gid)
{
	std::lock_guard lock(g_archiveMissLock);
	g_archiveMisses.erase({depotId, gid});
}

struct BodySink { std::string* out; std::size_t limit; };

std::size_t curlWriteCb(const char* p, std::size_t sz, std::size_t n, BodySink* sink)
{
	if (sz && n > SIZE_MAX / sz) return 0;
	const std::size_t bytes = sz * n;
	if (bytes > sink->limit - sink->out->size()) return 0;
	sink->out->append(p, bytes);
	return bytes;
}

#include <dlfcn.h>

typedef CURL* (*curl_easy_init_t)();
typedef CURLcode (*curl_easy_setopt_t)(CURL *curl, CURLoption option, ...);
typedef CURLcode (*curl_easy_perform_t)(CURL *curl);
typedef void (*curl_easy_cleanup_t)(CURL *curl);
typedef CURLcode (*curl_easy_getinfo_t)(CURL *curl, CURLINFO info, ...);
typedef const char* (*curl_easy_strerror_t)(CURLcode);
typedef curl_slist* (*curl_slist_append_t)(curl_slist*, const char*);
typedef void (*curl_slist_free_all_t)(curl_slist*);

static curl_easy_init_t p_curl_easy_init = nullptr;
static curl_easy_setopt_t p_curl_easy_setopt = nullptr;
static curl_easy_perform_t p_curl_easy_perform = nullptr;
static curl_easy_cleanup_t p_curl_easy_cleanup = nullptr;
static curl_easy_getinfo_t p_curl_easy_getinfo = nullptr;
static curl_easy_strerror_t p_curl_easy_strerror = nullptr;
static curl_slist_append_t p_curl_slist_append = nullptr;
static curl_slist_free_all_t p_curl_slist_free_all = nullptr;
static std::mutex g_curlLoadMutex;

static bool load_curl() {
	std::lock_guard lock(g_curlLoadMutex);
	if (p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform
	    && p_curl_easy_cleanup && p_curl_slist_append
	    && p_curl_slist_free_all)
	{
		return true;
	}

	void* handle = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!handle) handle = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!handle) handle = RTLD_DEFAULT;

	const auto init = (curl_easy_init_t)dlsym(handle, "curl_easy_init");
	const auto setopt = (curl_easy_setopt_t)dlsym(handle, "curl_easy_setopt");
	const auto perform = (curl_easy_perform_t)dlsym(handle, "curl_easy_perform");
	const auto cleanup = (curl_easy_cleanup_t)dlsym(handle, "curl_easy_cleanup");
	const auto getinfo = (curl_easy_getinfo_t)dlsym(handle, "curl_easy_getinfo");
	const auto strerror = (curl_easy_strerror_t)dlsym(handle, "curl_easy_strerror");
	const auto listAppend =
		(curl_slist_append_t)dlsym(handle, "curl_slist_append");
	const auto listFree =
		(curl_slist_free_all_t)dlsym(handle, "curl_slist_free_all");
	if (!init || !setopt || !perform || !cleanup || !getinfo || !strerror ||
	    !listAppend || !listFree)
		return false;

	p_curl_easy_init = init;
	p_curl_easy_setopt = setopt;
	p_curl_easy_perform = perform;
	p_curl_easy_cleanup = cleanup;
	p_curl_easy_getinfo = getinfo;
	p_curl_easy_strerror = strerror;
	p_curl_slist_append = listAppend;
	p_curl_slist_free_all = listFree;
	return true;
}

struct TransferControl
{
	const JobBudget* budget;
	const std::atomic<bool>* keepRunning;
};

int curlTransferProgress(void* userdata,
                         curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const auto* control = static_cast<const TransferControl*>(userdata);
	if (!control) return 0;
	if (control->budget && control->budget->shouldStop()) return 1;
	return control->keepRunning
	       && !control->keepRunning->load(std::memory_order_acquire) ? 1 : 0;
}

HttpResponse httpGet(const std::string& url, const JobBudget* budget = nullptr,
                     std::string_view hostHeader = {},
                     std::string_view method = "GET", std::string_view postBody = {},
                     std::size_t maxBodyBytes = 64u * 1024u * 1024u,
                     long timeoutMs = 10000,
                     const std::atomic<bool>* keepRunning = nullptr)
{
	HttpResponse r;
	if ((budget != nullptr && budget->shouldStop()) ||
	    (keepRunning && !keepRunning->load(std::memory_order_acquire)))
	{
		r.networkError = true;
		r.diagnostic = "manifest job budget expired";
		return r;
	}

	if (!load_curl())
	{
		r.networkError = true;
		r.diagnostic = "failed to load libcurl dynamically";
		return r;
	}

	CURL* c = p_curl_easy_init();
	if (!c)
	{
		r.networkError = true;
		r.diagnostic = "curl_easy_init failed";
		return r;
	}
	curl_slist* requestHeaders = nullptr;
	BodySink sink{&r.body, maxBodyBytes};
	if (!hostHeader.empty())
	{
		const std::string host = "Host: " + std::string(hostHeader);
		requestHeaders = p_curl_slist_append(nullptr, host.c_str());
		if (!requestHeaders)
		{
			p_curl_easy_cleanup(c);
			r.networkError = true;
			r.diagnostic = "failed to allocate curl Host header";
			return r;
		}
		p_curl_easy_setopt(c, CURLOPT_HTTPHEADER, requestHeaders);
	}
	p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, method == "POST" ? 0L : 1L);
	p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
	p_curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	if (method == "HEAD") p_curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
	if (method == "POST")
	{
		p_curl_easy_setopt(c, CURLOPT_POST, 1L);
		p_curl_easy_setopt(c, CURLOPT_POSTFIELDS, postBody.data());
		p_curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE,
		                   static_cast<curl_off_t>(postBody.size()));
		requestHeaders = p_curl_slist_append(requestHeaders, "Content-Type: text/plain");
		if (requestHeaders) p_curl_easy_setopt(c, CURLOPT_HTTPHEADER, requestHeaders);
	}
	if (budget == nullptr)
	{
		p_curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
		p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	}
	else
	{
		long long remainingMs = budget->remaining().count();
		if (remainingMs <= 0)
		{
			if (requestHeaders) p_curl_slist_free_all(requestHeaders);
			p_curl_easy_cleanup(c);
			r.networkError = true;
			r.diagnostic = "manifest job budget expired";
			return r;
		}
		if (remainingMs > timeoutMs) remainingMs = timeoutMs;
		const long timeoutMs = static_cast<long>(remainingMs < 1 ? 1 : remainingMs);
		const long connectMs = timeoutMs < 5000 ? timeoutMs : 5000;
		p_curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
		p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, connectMs);
	}
	TransferControl control{budget, keepRunning};
	if (budget || keepRunning)
	{
		p_curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		p_curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlTransferProgress);
		p_curl_easy_setopt(c, CURLOPT_XFERINFODATA, &control);
	}
	// MANDATORY for multi-threaded use: httpGet runs on a ManifestFetch
	// worker thread.  Without CURLOPT_NOSIGNAL, libcurl built with a
	// synchronous resolver implements timeouts via SIGALRM + siglongjmp.
	// That handler is process-wide; if SIGALRM fires while another thread
	// (e.g. Steam's main thread in poll()) is running, the longjmp targets
	// the wrong stack and glibc's __longjmp_chk aborts the whole client.
	// NOSIGNAL switches libcurl to signal-free timeouts.
	p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-ManifestFetch/0.1");
	// Pin the system trust store (see cainfo.hpp) so the https manifest
	// providers verify on SteamOS/Arch; no-op when no bundle is found.
	if (const char* f = ca::bundleFile()) p_curl_easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  p_curl_easy_setopt(c, CURLOPT_CAPATH, d);
	const CURLcode rc = p_curl_easy_perform(c);
	if (rc != CURLE_OK)
	{
		r.networkError = true;
		r.diagnostic = p_curl_easy_strerror ? p_curl_easy_strerror(rc) : "curl error";
	}
	else
	{
		r.networkError = false;
		r.diagnostic = "OK";
		if (p_curl_easy_getinfo) p_curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	}
	if (requestHeaders) p_curl_slist_free_all(requestHeaders);
	p_curl_easy_cleanup(c);
	return r;
}

} // namespace


namespace
{

std::string findSteamRootForBlob()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	const std::vector<std::string> candidates = {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
	for (const auto& candidate : candidates)
	{
		struct stat st{};
		if (stat((candidate + "/steam.sh").c_str(), &st) == 0)
		{
			return candidate;
		}
	}
	return {};
}

uint32_t appManifestState(const std::string& steamRoot, uint32_t appId)
{
	if (steamRoot.empty() || !appId) return 0;
	std::ifstream input(steamRoot + "/steamapps/appmanifest_"
	                    + std::to_string(appId) + ".acf");
	if (!input) return 0;
	std::string line;
	while (std::getline(input, line))
	{
		const auto key = line.find("\"StateFlags\"");
		if (key == std::string::npos) continue;
		const auto open = line.find('"', key + sizeof("\"StateFlags\"") - 1);
		if (open == std::string::npos) return 0;
		const auto close = line.find('"', open + 1);
		if (close == std::string::npos) return 0;
		uint32_t state = 0;
		const char* begin = line.data() + open + 1;
		const char* end = line.data() + close;
		const auto parsed = std::from_chars(begin, end, state);
		return parsed.ec == std::errc{} && parsed.ptr == end ? state : 0;
	}
	return 0;
}

bool writeManifestFile(const std::string& path, const void* data,
                       std::size_t size)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	std::size_t offset = 0;
	while (offset < size)
	{
		const ssize_t written = write(fd,
		    static_cast<const unsigned char*>(data) + offset, size - offset);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0)
		{
			close(fd);
			unlink(path.c_str());
			return false;
		}
		offset += static_cast<std::size_t>(written);
	}
	return close(fd) == 0;
}

bool fetchManifestBlob(uint64_t gid, uint32_t appId, uint32_t depotId,
                       const std::string& depotcacheDir,
                       const std::shared_ptr<JobBudget>& budget,
                       bool bypassArchiveMiss, uint64_t sessionEpoch)
{
	// appId is part of the shared blob API but not needed here: the archive is
	// addressed by depot and gid only.
	(void)appId;
	if (budget && budget->shouldStop()) return false;
	std::string targetPath = depotcacheDir + "/" + std::to_string(depotId)
	                          + "_" + std::to_string(gid) + ".manifest";
	if (ManifestStore::isInDepotcache(depotId, gid))
	{
		// Legacy/current-session files may predate write-through. Archive
		// this exact gid without scanning depotcache so a later Steam
		// purge cannot remove the only copy.
		ManifestStore::archiveManifest(depotId, gid);
		g_pLog->debug("ManifestFetch: blob depot=%u gid=%llu already at %s\n",
		              depotId,
		              static_cast<unsigned long long>(gid),
		              targetPath.c_str());
		return true;
	}

	// A non-empty but invalid file must not suppress a fresh provider fetch.
	// Steam may leave a truncated/corrupt depotcache entry after interruption.
	unlink(targetPath.c_str());

	enum class ArchiveResult { Failed, NotReady, Staged };
	const auto fetchArchive = [&]() -> ArchiveResult
	{
		const std::string archiveUrl = "https://manifest.luastools.xyz/m/"
		    + std::to_string(depotId) + "/" + std::to_string(gid);
		long archiveTimeout = 60000;
		if (budget)
			archiveTimeout = std::max<long>(1, std::min<long>(archiveTimeout,
			    static_cast<long>(budget->remaining().count())));
		const auto archived = httpGet(archiveUrl, budget.get(), {}, "GET", {},
		                              64u * 1024u * 1024u, archiveTimeout);
		// Feed the sole manifest provider's reachability into the offline
		// circuit. The fetch always runs (the user needs the manifest); the
		// circuit only records whether the archive host answered. A 404 still
		// counts as reachable — the manifest just has not been donated yet.
		if (const uint64_t token = beginProviderAttempt(sessionEpoch))
		{
			const auto outcome = classifyArchiveOutcome(archived.networkError,
			                                            archived.status);
			finishProviderAttempt(sessionEpoch, token, outcome.success,
			                      outcome.rateLimited, outcome.transportFailure);
		}
		if (!archived.networkError && archived.status == 404)
		{
			rememberArchiveMiss(depotId, gid);
			return ArchiveResult::NotReady;
		}
		if (!archived.networkError && archived.status == 200 &&
		    looksLikeArchivedManifest(archived.body))
		{
			clearArchiveMiss(depotId, gid);
			const std::string tmp = targetPath + ".archive." + std::to_string(getpid());
			if (writeManifestFile(tmp, archived.body.data(), archived.body.size()) &&
			    ManifestStore::publishDownloadedManifest(depotId, gid, tmp))
			{
				unlink(tmp.c_str());
				g_pLog->info("ManifestFetch: archive staged depot=%u gid=%llu\n",
			             depotId, static_cast<unsigned long long>(gid));
				return ArchiveResult::Staged;
			}
			unlink(tmp.c_str());
		}
		return ArchiveResult::Failed;
	};

	// The archive serves raw Steam manifest bytes. Reuse the durable store's
	// atomic publication path so Steam's depotcache purge cannot lose this copy.
	ArchiveResult archiveResult = ArchiveResult::Failed;
	if (bypassArchiveMiss ||
	    !hasRecentArchiveMiss(depotId, gid))
	{
		archiveResult = fetchArchive();
		if (archiveResult == ArchiveResult::Staged) return true;
	}
	auto failed = [&]
	{
		if (archiveResult == ArchiveResult::NotReady &&
		    bypassArchiveMiss &&
		    markMissingForNotification(depotId, gid, sessionEpoch))
			g_pLog->notifyUser(UserMsg::ManifestNotReady);
		return false;
	};

	// The luastools archive serves the whole manifest (fetchArchive above); it
	// is the only source. When it does not hold this gid yet there is nowhere
	// else to fetch it, so surface the miss and let Steam's own path / the
	// resilience fallback (feats/manifestbind.cpp) proceed from a local copy.
	return failed();
}

} // namespace

bool isAnyManagedDownloadActive(uint32_t appIdHint)
{
	auto apps = g_config.managedAppIds.get();
	if (appIdHint) apps.insert(appIdHint);
	if (apps.empty()) return false;
	const std::string steamRoot = findSteamRootForBlob();
	for (const uint32_t appId : apps)
	{
		if (g_pClientAppManager)
		{
			const auto state = static_cast<uint32_t>(
				g_pClientAppManager->getAppInstallState(appId));
			if (appStateIsDownloading(state)) return true;
		}
		if (appStateIsDownloading(appManifestState(steamRoot, appId))) return true;
	}
	return false;
}

namespace
{
// Dedupe blob downloads by (gid, depotId).  All three call sites
// (pics.cpp PICS recv, manifestcode.cpp Send/BYldRequestDepotManifest)
// can race for the same depot+gid; without dedupe each spawned its own
// thread and they raced over the same tmp file, causing the spurious
// "bad magic 0x0" warn (popup) seen in the logs.
struct BlobKey { uint64_t gid; uint32_t depotId; };
struct BlobKeyHash
{
	std::size_t operator()(const BlobKey& k) const noexcept
	{
		return std::hash<uint64_t>{}(k.gid) ^ (std::hash<uint32_t>{}(k.depotId) << 1);
	}
};
struct BlobKeyEq
{
	bool operator()(const BlobKey& a, const BlobKey& b) const noexcept
	{
		return a.gid == b.gid && a.depotId == b.depotId;
	}
};

struct BlobJob
{
	std::shared_future<bool> future;
	std::shared_ptr<JobBudget> budget;
	uint64_t sessionEpoch = 0;
	std::atomic<int> waiters{0};
	std::atomic<uint32_t> appId{0};
	std::atomic<bool> bypassArchiveMiss{false};
	// A fire-and-forget submit owns the job independently of blocking
	// waiters.  A timed-out waiter may leave, but must not cancel the producer
	// while it is still staging the manifest in the background.
	bool producerActive = true;
};

std::mutex g_blobLock;
std::unordered_map<BlobKey, std::shared_ptr<BlobJob>, BlobKeyHash, BlobKeyEq> g_blobInflight;

// Fixed concurrency and a finite queue. Each job also has a shared deadline,
// so a queued manifest expires before it can consume a worker indefinitely.
BoundedExecutor& blobExecutor()
{
	static auto* executor = new BoundedExecutor(8, 64);
	return *executor;
}

std::shared_ptr<BlobJob> launchOrJoinBlob(uint64_t gid, uint32_t appId,
	                                      uint32_t depotId,
                                          const std::string& depotcacheDir,
                                          bool registerWaiter,
                                          bool bypassArchiveMiss)
{
	const BlobKey key{gid, depotId};
	std::lock_guard<std::mutex> lk(g_blobLock);
	const uint64_t sessionEpoch =
		g_sessionEpoch.load(std::memory_order_acquire);
	auto it = g_blobInflight.find(key);
	if (it != g_blobInflight.end() &&
	    it->second->sessionEpoch != sessionEpoch)
	{
		it->second->budget->cancel();
		g_blobInflight.erase(it);
		it = g_blobInflight.end();
	}
	if (it != g_blobInflight.end())
	{
		auto job = it->second;
		detail::promoteAppId(job->appId, appId);
		detail::promoteArchiveMissBypass(job->bypassArchiveMiss,
		                                bypassArchiveMiss);
		// If the previous job finished successfully AND the manifest is
		// still on disk, return that. If it finished but failed, OR the
		// file is gone, drop the entry so a fresh re-fetch happens.
		if (job->future.wait_for(std::chrono::seconds(0)) ==
		    std::future_status::ready)
		{
			const bool onDisk = ManifestStore::isInDepotcache(depotId, gid);
			if (job->future.get() && onDisk)
			{
				if (registerWaiter) detail::registerWaiterLocked(job->waiters);
				return job;
			}
			g_blobInflight.erase(it);
		}
		else
		{
			if (registerWaiter) detail::registerWaiterLocked(job->waiters);
			return job;
		}
	}

	auto completion = std::make_shared<std::promise<bool>>();
	auto job = std::make_shared<BlobJob>();
	job->future = completion->get_future().share();
	job->budget = std::make_shared<JobBudget>(
		std::chrono::seconds(60));
	job->sessionEpoch = sessionEpoch;
	detail::promoteAppId(job->appId, appId);
	detail::promoteArchiveMissBypass(job->bypassArchiveMiss,
	                                bypassArchiveMiss);
	if (registerWaiter) detail::registerWaiterLocked(job->waiters);
	g_blobInflight.emplace(key, job);

	const bool accepted = blobExecutor().submit(
	    [gid, depotId, depotcacheDir, completion, job, key]
	    {
	        bool ok = false;
	        bool activeRetryDone = false;
	        for (;;)
	        {
	            try
	            {
	                if (!job->budget->shouldStop())
	                {
	                    // Do filesystem restoration and any network fetch on our
	                    // executor, never on Steam's PICS/IPC worker.
	                    const bool bypassArchiveMiss =
	                        job->bypassArchiveMiss.load(std::memory_order_acquire);
	                    const bool firstAttempt = !activeRetryDone;
	                    if (bypassArchiveMiss) activeRetryDone = true;
	                    ok = (firstAttempt &&
	                          ManifestStore::restoreToDepotcache(depotId, gid))
	                         || fetchManifestBlob(
	                             gid,
	                             job->appId.load(std::memory_order_acquire),
	                             depotId, depotcacheDir, job->budget,
	                             bypassArchiveMiss, job->sessionEpoch);
	                }
	            }
	            catch (...)
	            {
	                g_pLog->info(
	                    "ManifestFetch: blob depot=%u gid=%llu worker failed unexpectedly\n",
	                    depotId, static_cast<unsigned long long>(gid));
	            }

	            // Serialize the terminal decision with join/promotion. If an
	            // active installer promoted this background job before we took
	            // the lock, make one final archive-bypass attempt. A joiner that
	            // arrives after this lock sees the completed job removed and
	            // starts its own active attempt, so no promotion can be lost.
	            std::unique_lock<std::mutex> lk(g_blobLock);
	            if (!ok && !activeRetryDone && !job->budget->shouldStop() &&
	                job->bypassArchiveMiss.load(std::memory_order_acquire))
	            {
	                activeRetryDone = true;
	                lk.unlock();
	                continue;
	            }
	            try { completion->set_value(ok); } catch (...) {}
	            job->producerActive = false;
	            auto it = g_blobInflight.find(key);
	            if (it != g_blobInflight.end() && it->second == job)
	                g_blobInflight.erase(it);
	            break;
	        }
	    });
	if (!accepted)
	{
		g_pLog->info(
		    "ManifestFetch: blob depot=%u gid=%llu executor queue unavailable\n",
		    depotId, static_cast<unsigned long long>(gid));
		job->budget->cancel();
		try { completion->set_value(false); } catch (...) {}
		job->producerActive = false;
		g_blobInflight.erase(key);
	}
	return job;
}

void releaseBlobWaiter(const BlobKey& key, const std::shared_ptr<BlobJob>& job,
                       bool cancelIfLast)
{
	std::lock_guard<std::mutex> lk(g_blobLock);
	if (!detail::releaseWaiterLocked(job->waiters)) return;
	if (!detail::shouldCancelBlobJob(
		        job->producerActive, /*lastWaiter=*/true, cancelIfLast))
		return;

	auto it = g_blobInflight.find(key);
	if (it != g_blobInflight.end() && it->second == job)
		g_blobInflight.erase(it);
	job->budget->cancel();
}
} // namespace



int getTimeoutSec()
{
	return 12;
}

const char* defaultTimeoutKey()
{
	return "ManifestFetch.timeout_sec";
}

void submitManifestBlob(uint64_t manifestGid, uint32_t appId, uint32_t depotId,
                        bool bypassArchiveMiss)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty())
	{
		g_pLog->debug("ManifestFetch: blob depot=%u gid=%llu skip, no Steam root\n",
		              depotId,
		              static_cast<unsigned long long>(manifestGid));
		return;
	}
	const std::string depotcacheDir = steamRoot + "/depotcache";
	(void)launchOrJoinBlob(manifestGid, appId, depotId, depotcacheDir,
	                       /*registerWaiter=*/false, bypassArchiveMiss);
}

bool awaitManifestBlob(uint64_t manifestGid, uint32_t appId,
                       uint32_t depotId, int timeoutSec, bool activeRequest)
{
	if (timeoutSec <= 0) timeoutSec = getTimeoutSec();
	return awaitManifestBlobFor(
	    manifestGid, appId, depotId, timeoutSec * 1000,
	    /*notifyOnTimeout=*/true,
	    activeRequest);
}

bool awaitManifestBlobFor(uint64_t manifestGid, uint32_t appId,
                          uint32_t depotId,
                          int timeoutMs, bool notifyOnTimeout,
                          bool activeRequest)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty()) return false;
	const std::string depotcacheDir = steamRoot + "/depotcache";
	auto job = launchOrJoinBlob(manifestGid, appId, depotId, depotcacheDir,
	                            /*registerWaiter=*/true,
	                            /*bypassArchiveMiss=*/activeRequest);
	if (timeoutMs < 0) timeoutMs = 0;
	if (job->future.wait_for(std::chrono::milliseconds(timeoutMs)) !=
	    std::future_status::ready)
	{
		releaseBlobWaiter(BlobKey{manifestGid, depotId}, job,
		                   /*cancelIfLast=*/true);
		g_pLog->info(
		    "ManifestFetch: blob depot=%u gid=%llu await timed out after %dms\n",
		    depotId, static_cast<unsigned long long>(manifestGid), timeoutMs);
		if (notifyOnTimeout) g_pLog->notifyUser(UserMsg::DownloadTimedOut);
		return false;
	}
	const bool result = job->future.get();
	releaseBlobWaiter(BlobKey{manifestGid, depotId}, job,
	                   /*cancelIfLast=*/false);
	return result;
}

bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t appId,
                           uint32_t depotId)
{
	return awaitManifestBlob(manifestGid, appId, depotId, getTimeoutSec());
}

void resetSessionState()
{
	{
		std::lock_guard circuitLock(g_circuitLock);
		g_sessionEpoch.fetch_add(1, std::memory_order_acq_rel);
		g_providerCircuit.cancelCurrentAttempt();
	}
	{
		std::lock_guard lock(g_missingNotifyLock);
		g_missingNotified.clear();
	}
	{
		std::lock_guard lock(g_blobLock);
		for (const auto& [key, job] : g_blobInflight)
			job->budget->cancel();
		g_blobInflight.clear();
	}
}

bool areProvidersOffline()
{
	return g_providersOffline.load();
}

HttpResponse archiveRequest(const std::string& method, const std::string& url,
	                        const std::string& body, std::size_t maxBodyBytes,
	                        long timeoutMs,
	                        const std::atomic<bool>* keepRunning)
{
	if (method != "GET" && method != "HEAD" && method != "POST")
		return {0, {}, true, "unsupported HTTP method"};
	return httpGet(url, nullptr, {}, method, body, maxBodyBytes, timeoutMs,
	               keepRunning);
}

} // namespace ManifestFetch
