
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../feats/manifeststore.hpp"
#include "../log.hpp"
#include "../cainfo.hpp"
#include "boundedexecutor.hpp"
#include "../sdk/IClientAppManager.hpp"

#include "curl_api.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>


namespace ManifestFetch
{

namespace
{

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

	if (!curlApi())
	{
		r.networkError = true;
		r.diagnostic = "failed to load libcurl dynamically";
		return r;
	}

	CURL* c = curlApi()->easy_init();
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
		requestHeaders = curlApi()->slist_append(nullptr, host.c_str());
		if (!requestHeaders)
		{
			curlApi()->easy_cleanup(c);
			r.networkError = true;
			r.diagnostic = "failed to allocate curl Host header";
			return r;
		}
		curlApi()->easy_setopt(c, CURLOPT_HTTPHEADER, requestHeaders);
	}
	curlApi()->easy_setopt(c, CURLOPT_URL, url.c_str());
	curlApi()->easy_setopt(c, CURLOPT_FOLLOWLOCATION, method == "POST" ? 0L : 1L);
	// A redirect never downgrades an https request to plain http.
	curlApi()->easy_setopt(c, CURLOPT_REDIR_PROTOCOLS,
	                       url.rfind("https://", 0) == 0
	                           ? static_cast<long>(CURLPROTO_HTTPS)
	                           : static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curlApi()->easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	curlApi()->easy_setopt(c, CURLOPT_WRITEDATA, &sink);
	curlApi()->easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	if (method == "HEAD") curlApi()->easy_setopt(c, CURLOPT_NOBODY, 1L);
	if (method == "POST")
	{
		curlApi()->easy_setopt(c, CURLOPT_POST, 1L);
		curlApi()->easy_setopt(c, CURLOPT_POSTFIELDS, postBody.data());
		curlApi()->easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE,
		                   static_cast<curl_off_t>(postBody.size()));
		requestHeaders = curlApi()->slist_append(requestHeaders, "Content-Type: text/plain");
		if (requestHeaders) curlApi()->easy_setopt(c, CURLOPT_HTTPHEADER, requestHeaders);
	}
	if (budget == nullptr)
	{
		curlApi()->easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
		curlApi()->easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	}
	else
	{
		long long remainingMs = budget->remaining().count();
		if (remainingMs <= 0)
		{
			if (requestHeaders) curlApi()->slist_free_all(requestHeaders);
			curlApi()->easy_cleanup(c);
			r.networkError = true;
			r.diagnostic = "manifest job budget expired";
			return r;
		}
		if (remainingMs > timeoutMs) remainingMs = timeoutMs;
		const long budgetMs = static_cast<long>(remainingMs < 1 ? 1 : remainingMs);
		const long connectMs = budgetMs < 5000 ? budgetMs : 5000;
		curlApi()->easy_setopt(c, CURLOPT_TIMEOUT_MS, budgetMs);
		curlApi()->easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, connectMs);
	}
	TransferControl control{budget, keepRunning};
	if (budget || keepRunning)
	{
		curlApi()->easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curlApi()->easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlTransferProgress);
		curlApi()->easy_setopt(c, CURLOPT_XFERINFODATA, &control);
	}
	// MANDATORY for multi-threaded use: httpGet runs on a ManifestFetch
	// worker thread.  Without CURLOPT_NOSIGNAL, libcurl built with a
	// synchronous resolver implements timeouts via SIGALRM + siglongjmp.
	// That handler is process-wide; if SIGALRM fires while another thread
	// (e.g. Steam's main thread in poll()) is running, the longjmp targets
	// the wrong stack and glibc's __longjmp_chk aborts the whole client.
	// NOSIGNAL switches libcurl to signal-free timeouts.
	curlApi()->easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curlApi()->easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-ManifestFetch/0.1");
	// Pin the system trust store (see cainfo.hpp) so the https manifest
	// providers verify on SteamOS/Arch; no-op when no bundle is found.
	if (const char* f = ca::bundleFile()) curlApi()->easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  curlApi()->easy_setopt(c, CURLOPT_CAPATH, d);
	const CURLcode rc = curlApi()->easy_perform(c);
	if (rc != CURLE_OK)
	{
		r.networkError = true;
		r.diagnostic = curlApi()->easy_strerror(rc);
	}
	else
	{
		r.networkError = false;
		r.diagnostic = "OK";
		curlApi()->easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	}
	if (requestHeaders) curlApi()->slist_free_all(requestHeaders);
	curlApi()->easy_cleanup(c);
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
	const std::string steamRoot = findSteamRootForBlob();
	const auto downloading = [&steamRoot](uint32_t appId)
	{
		if (g_pClientAppManager &&
		    appStateIsDownloading(static_cast<uint32_t>(
			    g_pClientAppManager->getAppInstallState(appId))))
			return true;
		return appStateIsDownloading(appManifestState(steamRoot, appId));
	};
	if (appIdHint && downloading(appIdHint)) return true;
	// Asked once per depot while Steam plans; the fleet scan reads one
	// appmanifest per managed app, so it runs once per window.
	static DownloadActivityCache fleet;
	return fleet.get(steadyNowMs(), [&]
	{
		for (const uint32_t appId : g_config.managedAppIds.get())
			if (downloading(appId)) return true;
		return false;
	});
}

namespace
{
// Jobs are deduplicated by (gid, depot): concurrent callers for the same
// manifest share one download and one temporary file.
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
	std::atomic<uint32_t> appId{0};
	std::atomic<bool> bypassArchiveMiss{false};
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
				return job;
			g_blobInflight.erase(it);
		}
		else
		{
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
	                    const bool bypass =
	                        job->bypassArchiveMiss.load(std::memory_order_acquire);
	                    const bool firstAttempt = !activeRetryDone;
	                    if (bypass) activeRetryDone = true;
	                    ok = (firstAttempt &&
	                          ManifestStore::restoreToDepotcache(depotId, gid))
	                         || fetchManifestBlob(
	                             gid,
	                             job->appId.load(std::memory_order_acquire),
	                             depotId, depotcacheDir, job->budget,
	                             bypass, job->sessionEpoch);
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
	            std::unique_lock<std::mutex> lock(g_blobLock);
	            if (!ok && !activeRetryDone && !job->budget->shouldStop() &&
	                job->bypassArchiveMiss.load(std::memory_order_acquire))
	            {
	                activeRetryDone = true;
	                lock.unlock();
	                continue;
	            }
	            try { completion->set_value(ok); } catch (...) {}
	            auto entry = g_blobInflight.find(key);
	            if (entry != g_blobInflight.end() && entry->second == job)
	                g_blobInflight.erase(entry);
	            break;
	        }
	    });
	if (!accepted)
	{
		g_pLog->debug(
		    "ManifestFetch: blob depot=%u gid=%llu executor queue unavailable\n",
		    depotId, static_cast<unsigned long long>(gid));
		job->budget->cancel();
		try { completion->set_value(false); } catch (...) {}
		g_blobInflight.erase(key);
	}
	return job;
}

} // namespace



int getTimeoutSec()
{
	return 12;
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
	                       bypassArchiveMiss);
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
	                            /*bypassArchiveMiss=*/activeRequest);
	if (timeoutMs < 0) timeoutMs = 0;
	if (job->future.wait_for(std::chrono::milliseconds(timeoutMs)) !=
	    std::future_status::ready)
	{
		g_pLog->debug(
		    "ManifestFetch: blob depot=%u gid=%llu await timed out after %dms\n",
		    depotId, static_cast<unsigned long long>(manifestGid), timeoutMs);
		if (notifyOnTimeout) g_pLog->notifyUser(UserMsg::DownloadTimedOut);
		return false;
	}
	return job->future.get();
}


void clearOfflineFlags()
{
	const char* home = std::getenv("HOME");
	if (!home) return;
	const std::filesystem::path dir = std::filesystem::path(home) / ".config/SLSsteam";
	std::error_code ec;
	std::filesystem::remove(dir / "offline", ec);
	for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
	     it.increment(ec))
	{
		std::error_code entryEc;
		if (it->path().filename().string().starts_with("offline_")
		    && it->is_regular_file(entryEc))
		{
			std::filesystem::remove(it->path(), entryEc);
		}
	}
}

void resetSessionState()
{
	{
		std::lock_guard circuitLock(g_circuitLock);
		g_sessionEpoch.fetch_add(1, std::memory_order_acq_rel);
		// A new session starts with the archive assumed reachable.
		g_providerCircuit.reset();
	}
	setOfflineStatus(false);
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
	std::lock_guard<std::mutex> lock(g_circuitLock);
	return g_providerCircuit.coolingDown(steadyNowMs());
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
