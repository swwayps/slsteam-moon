
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../feats/manifeststore.hpp"
#include "../log.hpp"
#include "../cainfo.hpp"
#include "boundedexecutor.hpp"
#include "contentserverdirectory.hpp"
#include "manifest_zip.hpp"
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

std::mutex g_notFoundLock;
std::unordered_set<uint64_t> g_notFoundGids;

void markGidNotFoundInternal(uint64_t gid)
{
	std::lock_guard<std::mutex> lk(g_notFoundLock);
	g_notFoundGids.insert(gid);
}

bool isGidNotFoundInternal(uint64_t gid)
{
	std::lock_guard<std::mutex> lk(g_notFoundLock);
	return g_notFoundGids.count(gid) > 0;
}

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
RequestCodeCircuit g_requestCodeCircuit(
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
	return g_requestCodeCircuit.beginAttempt(steadyNowMs());
}

void cancelProviderAttempt(uint64_t attemptToken)
{
	std::lock_guard<std::mutex> lock(g_circuitLock);
	g_requestCodeCircuit.cancelAttempt(attemptToken);
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
			g_requestCodeCircuit.cancelAttempt(attemptToken);
			return;
		}
		wasOffline = g_requestCodeCircuit.open();
		g_requestCodeCircuit.finishAttempt(
		    attemptToken, steadyNowMs(), success, rateLimited, transportFailure);
		nowOffline = g_requestCodeCircuit.open();
	}
	g_providersOffline.store(nowOffline, std::memory_order_release);
	setOfflineStatus(nowOffline);
	if (!wasOffline && nowOffline)
	{
		g_pLog->info(
		    "ManifestFetch: request-code circuit opened; retrying a real gid after cooldown\n");
	}
	else if (wasOffline && !nowOffline)
	{
		g_pLog->info(
		    "ManifestFetch: real-gid request succeeded; request-code circuit closed\n");
	}
}

std::mutex g_lock;
struct PendingRequestCode
{
	uint64_t epoch;
	std::shared_future<std::optional<uint64_t>> future;
	std::shared_ptr<JobBudget> budget;
};
std::map<uint64_t, PendingRequestCode> g_pending;

BoundedExecutor& requestCodeExecutor()
{
	static auto* executor = new BoundedExecutor(8, 128);
	return *executor;
}

// One-use handoff for the paired consumers in a normal install: the blob fetch
// and Steam's GetManifestRequestCode response. Codes are depot-bound and
// short-lived, so keep them only in memory, consume them on lookup, and expire
// an orphan after one minute.
std::mutex g_codeLock;
struct CachedRequestCode
{
	uint64_t value;
	int64_t storedAtMs;
	uint64_t epoch;
};
std::map<std::pair<uint32_t, uint64_t>, CachedRequestCode> g_codeByDepotGid;

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

void cacheCode(uint32_t depotId, uint64_t gid, uint64_t code,
	           uint64_t epoch)
{
	if (!depotId || !gid || !code ||
	    epoch != g_sessionEpoch.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lk(g_codeLock);
	if (epoch != g_sessionEpoch.load(std::memory_order_acquire)) return;
	const int64_t now = steadyNowMs();
	std::erase_if(g_codeByDepotGid, [now](const auto& item)
	{
		return item.second.epoch !=
		           g_sessionEpoch.load(std::memory_order_acquire)
		       || !cachedRequestCodeIsFresh(now, item.second.storedAtMs);
	});
	if (g_codeByDepotGid.size() >= 4096) g_codeByDepotGid.clear();
	g_codeByDepotGid[{depotId, gid}] = {code, now, epoch};
}

std::optional<uint64_t> cachedCode(uint32_t depotId, uint64_t gid,
	                              uint64_t epoch)
{
	std::lock_guard<std::mutex> lk(g_codeLock);
	auto it = g_codeByDepotGid.find({depotId, gid});
	if (it == g_codeByDepotGid.end()) return std::nullopt;
	const auto cached = it->second;
	g_codeByDepotGid.erase(it);
	if (cached.epoch != epoch ||
	    epoch != g_sessionEpoch.load(std::memory_order_acquire) ||
	    !cachedRequestCodeIsFresh(steadyNowMs(), cached.storedAtMs))
		return std::nullopt;
	return cached.value;
}

bool parseDigitsOnly(std::string_view body, uint64_t* out)
{
	if (body.empty()) return false;
	std::size_t b = 0, e = body.size();
	auto isWs = [](char c)
	{
		return c == ' ' || c == '\r' || c == '\n' || c == '\t';
	};
	while (b < e && isWs(body[b])) ++b;
	while (e > b && isWs(body[e - 1])) --e;
	if (b == e) return false;
	for (std::size_t i = b; i < e; ++i)
	{
		if (body[i] < '0' || body[i] > '9') return false;
	}
	uint64_t v = 0;
	auto [_, ec] = std::from_chars(body.data() + b, body.data() + e, v);
	if (ec != std::errc{}) return false;
	*out = v;
	return true;
}

bool parseJsonDigitField(std::string_view body, uint64_t* out)
{
	static constexpr std::string_view kKeys[] =
	{
		"\"manifest_request_code\"",
		"\"content\"",
		"\"code\"",
	};
	for (auto key : kKeys)
	{
		auto k = body.find(key);
		if (k == std::string_view::npos) continue;
		auto q1 = body.find('"', k + key.size());
		if (q1 == std::string_view::npos) continue;
		auto q2 = body.find('"', q1 + 1);
		if (q2 == std::string_view::npos) continue;
		if (parseDigitsOnly(body.substr(q1 + 1, q2 - q1 - 1), out))
		{
			return true;
		}
	}
	return false;
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

std::mutex g_contentServerLock;
std::vector<ContentServerDirectory::Server> g_contentServers;
std::chrono::steady_clock::time_point g_contentServersExpire{};

std::vector<ContentServerDirectory::Server> contentServers(
    const std::shared_ptr<JobBudget>& budget)
{
	static constexpr std::string_view kDirectoryUrl =
	    "https://api.steampowered.com/"
	    "IContentServerDirectoryService/GetServersForSteamPipe/v1/"
	    "?cell_id=0&max_servers=20";
	static constexpr auto kCacheTtl = std::chrono::minutes(10);
	static constexpr auto kRetryTtl = std::chrono::minutes(1);

	std::lock_guard<std::mutex> lock(g_contentServerLock);
	const auto now = std::chrono::steady_clock::now();
	if (now < g_contentServersExpire)
	{
		return g_contentServers;
	}

	const auto response = httpGet(std::string(kDirectoryUrl), budget.get());
	if (!response.networkError && response.status == 200)
	{
		auto parsed = ContentServerDirectory::parseServerList(response.body);
		if (!parsed.empty())
		{
			g_contentServers = std::move(parsed);
			g_contentServersExpire = now + kCacheTtl;
			g_pLog->info(
			    "ManifestFetch: content directory returned %zu ranked server(s)\n",
			    g_contentServers.size());
			return g_contentServers;
		}
	}

	if (!g_contentServers.empty())
	{
		g_contentServersExpire = now + kRetryTtl;
		g_pLog->info(
		    "ManifestFetch: content directory refresh failed (HTTP=%ld err='%s'); "
		    "reusing %zu cached server(s)\n",
		    response.status, response.diagnostic.c_str(), g_contentServers.size());
		return g_contentServers;
	}

	g_pLog->info(
	    "ManifestFetch: content directory unavailable (HTTP=%ld err='%s')\n",
	    response.status, response.diagnostic.c_str());
	g_contentServersExpire = now + kRetryTtl;
	return {};
}

std::optional<uint64_t> runOnce(uint64_t gid, uint32_t appId, uint32_t depotId,
	                            uint64_t sessionEpoch,
                                const std::shared_ptr<JobBudget>& budget = {})
{
	const auto cancelled = [&]
	{
		return (budget && budget->shouldStop()) ||
		       sessionEpoch != g_sessionEpoch.load(std::memory_order_acquire);
	};
	if (cancelled())
		return std::nullopt;

	// Fast path: a previous resolve (e.g. the blob fetch in
	// BYldRequestDepotManifest) already learned this gid's request
	// code.  Reuse it so we don't race a fresh HTTP round-trip when
	// Steam's GetManifestRequestCode job response arrives.
	if (auto c = cachedCode(depotId, gid, sessionEpoch))
	{
		return c;
	}

	// An open circuit admits one actual queued gid after the cooldown. This is
	// both the recovery probe and useful work; a dead provider's gid=0 404 can
	// never reset health falsely.
	const uint64_t attemptToken = beginProviderAttempt(sessionEpoch);
	if (!attemptToken)
	{
		g_pLog->debug(
		    "ManifestFetch: gid=%llu skipped during request-code cooldown\n",
		    static_cast<unsigned long long>(gid));
		return std::nullopt;
	}
	if (cancelled())
	{
		cancelProviderAttempt(attemptToken);
		return std::nullopt;
	}

	const auto& chain = defaultProviderChain();
	if (chain.empty())
	{
		cancelProviderAttempt(attemptToken);
		g_pLog->debug("ManifestFetch: gid=%llu skipped, no providers configured\n",
		              static_cast<unsigned long long>(gid));
		return std::nullopt;
	}

	bool hasNetworkOrServerError = false;
	bool hasRateLimit = false;
	std::vector<ProviderOutcome> outcomes;
	outcomes.reserve(chain.size());
	for (std::size_t i = 0; i < chain.size(); ++i)
	{
		if (cancelled())
		{
			cancelProviderAttempt(attemptToken);
			return std::nullopt;
		}
		const auto& tmpl = chain[i];
		if (tmpl.empty()) continue;
		const auto url = expandProviderTemplate(tmpl, gid, appId, depotId);
		g_pLog->info("ManifestFetch: gid=%llu provider %zu/%zu GET %s\n",
		             static_cast<unsigned long long>(gid),
		             i + 1, chain.size(), url.c_str());

		const auto resp = httpGet(url, budget.get());
		if (cancelled())
		{
			cancelProviderAttempt(attemptToken);
			return std::nullopt;
		}
		if (resp.networkError)
		{
			outcomes.push_back({true, 0});
			g_pLog->info("ManifestFetch: gid=%llu provider %zu net err '%s', trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.diagnostic.c_str());
			hasNetworkOrServerError = true;
			continue;
		}
		if (resp.status != 200)
		{
			outcomes.push_back({false, resp.status});
			g_pLog->info("ManifestFetch: gid=%llu provider %zu HTTP=%ld body_bytes=%zu, trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.status, resp.body.size());
			if (resp.status == 429)
			{
				hasRateLimit = true;
			}
			else if (resp.status >= 500)
			{
				hasNetworkOrServerError = true;
			}
			continue;
		}
		outcomes.push_back({false, 200});
		uint64_t code = 0;
		if (parseDigitsOnly(resp.body, &code) || parseJsonDigitField(resp.body, &code))
		{
			g_pLog->info("ManifestFetch: gid=%llu resolved request code via provider %zu\n",
			             static_cast<unsigned long long>(gid),
			             i + 1);
			cacheCode(depotId, gid, code, sessionEpoch);
			finishProviderAttempt(sessionEpoch, attemptToken,
				/*success=*/true, /*rateLimited=*/false,
			    /*transportFailure=*/false);
			return code;
		}
		g_pLog->info("ManifestFetch: gid=%llu provider %zu body unparseable "
		             "(%zu bytes), trying next\n",
		             static_cast<unsigned long long>(gid),
		             i + 1, resp.body.size());
	}

	g_pLog->info("ManifestFetch: gid=%llu all %zu providers exhausted\n",
	             static_cast<unsigned long long>(gid), chain.size());
	if (isDefinitiveNotFound(outcomes))
		markGidNotFoundInternal(gid);

	finishProviderAttempt(sessionEpoch, attemptToken,
	    /*success=*/false, hasRateLimit, hasNetworkOrServerError);

	// No user notification here: when the request-code providers are down the
	// manifest resilience fallback (feats/manifestbind.cpp) installs from a
	// locally-staged/archived manifest, so the download proceeds for the user.
	// Keep it log-only.
	return std::nullopt;
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

	auto codeOpt = runOnce(gid, appId, depotId, sessionEpoch, budget);
	if (!codeOpt)
	{
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu request-code lookup failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		return failed();
	}
	uint64_t code = *codeOpt;

	// Ask Valve's content directory for a region/load-ranked list.  cell_id=0
	// lets the service choose for the requester's public IP, matching Steam's
	// normal content path without pinning users to one geographic cell.
	const auto servers = contentServers(budget);
	if (servers.empty()) return failed();

	HttpResponse zipResp;
	bool gotZip = false;
	for (const auto& server : servers)
	{
		if (budget && budget->shouldStop()) break;
		const std::string cdnUrl = ContentServerDirectory::manifestUrl(
		    server, depotId, gid, code);
		const std::string vhost = ContentServerDirectory::hostHeader(server);
		zipResp = httpGet(cdnUrl, budget.get(), vhost);
		if (!zipResp.networkError && zipResp.status == 200 && !zipResp.body.empty())
		{
			gotZip = true;
			break;
		}
		if (requiresSteamCdnAuth({zipResp.networkError, zipResp.status}))
		{
			g_pLog->infoOnce(
			    "ManifestFetch: CDN host requires Steam depot authentication; "
			    "trying alternate hosts for this manifest\n");
		}
		// info, not warn: warn fires a notify-send popup; a single edge
		// returning a transient error is expected and we just try the next host.
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu host=%s HTTP=%ld err='%s', trying next CDN\n",
		             depotId, static_cast<unsigned long long>(gid), server.host.c_str(),
		             zipResp.status, zipResp.diagnostic.c_str());
	}
	if (!gotZip)
	{
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu all CDN hosts failed (last HTTP=%ld)\n",
		             depotId, static_cast<unsigned long long>(gid), zipResp.status);
		// Log-only: this is our manifest-blob staging fetch; the resilience
		// fallback (feats/manifestbind.cpp) installs from a local manifest
		// when it fails, so the download proceeds.  A genuine content-CDN
		// outage (chunks unreachable) is surfaced by Steam's own UI.
		return failed();
	}

	std::vector<unsigned char> manifest;
	std::string extractDiagnostic;
	if (!ManifestZip::extractSingleFile(zipResp.body, manifest, &extractDiagnostic))
	{
		g_pLog->warn(
			"ManifestFetch: blob depot=%u gid=%llu archive rejected: %s\n",
			depotId, static_cast<unsigned long long>(gid),
			extractDiagnostic.c_str());
		return failed();
	}

	const std::string tmpOutPath = targetPath + ".slsteam_tmp." +
	                               std::to_string(static_cast<unsigned long>(getpid())) + "." +
	                               std::to_string(reinterpret_cast<uintptr_t>(&zipResp));
	if (manifest.size() < sizeof(std::uint32_t)
	    || manifest[0] != 0xd0 || manifest[1] != 0x17
	    || manifest[2] != 0xf6 || manifest[3] != 0x71)
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu bad manifest magic\n",
		             depotId, static_cast<unsigned long long>(gid));
		return failed();
	}
	if (!writeManifestFile(tmpOutPath, manifest.data(), manifest.size()))
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu manifest write failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return failed();
	}

	if (!ManifestStore::publishDownloadedManifest(depotId, gid, tmpOutPath))
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn(
		    "ManifestFetch: blob depot=%u gid=%llu persistent publish failed\n",
		    depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return failed();
	}
	unlink(tmpOutPath.c_str());

	g_pLog->info("ManifestFetch: blob depot=%u gid=%llu staged at %s\n",
	             depotId, static_cast<unsigned long long>(gid),
	             targetPath.c_str());
	return true;
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

void submit(uint64_t jobId, uint64_t manifestGid, uint32_t appId, uint32_t depotId)
{
	auto completion =
		std::make_shared<std::promise<std::optional<uint64_t>>>();
	const auto future = completion->get_future().share();
	const int timeoutSecs = getTimeoutSec() > 0 ? getTimeoutSec() : 12;
	auto budget = std::make_shared<JobBudget>(
		std::chrono::seconds(timeoutSecs));
	uint64_t sessionEpoch = 0;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		if (g_pending.count(jobId))
		{
			g_pLog->debug("ManifestFetch: duplicate submit for jobId=%llu\n",
			              static_cast<unsigned long long>(jobId));
			return;
		}
		sessionEpoch = g_sessionEpoch.load(std::memory_order_acquire);
		g_pending.emplace(
			jobId, PendingRequestCode{sessionEpoch, future, budget});
	}
	const bool accepted = requestCodeExecutor().submit(
		[completion, manifestGid, appId, depotId, sessionEpoch, budget]
	{
			std::optional<uint64_t> result;
			try
			{
				result = runOnce(
					manifestGid, appId, depotId, sessionEpoch, budget);
			}
			catch (...) {}
			try { completion->set_value(result); } catch (...) {}
		});
	if (!accepted)
	{
		budget->cancel();
		try { completion->set_value(std::nullopt); } catch (...) {}
	}
}

std::optional<uint64_t> resolve(uint64_t jobId)
{
	PendingRequestCode pending{};
	{
		std::lock_guard<std::mutex> lk(g_lock);
		auto it = g_pending.find(jobId);
		if (it == g_pending.end()) return std::nullopt;
		pending = it->second;
	}
	if (pending.epoch != g_sessionEpoch.load(std::memory_order_acquire))
	{
		pending.budget->cancel();
		return std::nullopt;
	}
	const int budget = getTimeoutSec() > 0 ? getTimeoutSec() : 12;
	if (pending.future.wait_for(std::chrono::seconds(budget)) !=
	    std::future_status::ready)
	{
		pending.budget->cancel();
		std::lock_guard<std::mutex> lk(g_lock);
		g_pending.erase(jobId);
		g_pLog->info("ManifestFetch: jobId=%llu timed out after %ds\n",
		             static_cast<unsigned long long>(jobId), budget);
		g_pLog->notifyUser(UserMsg::DownloadTimedOut);
		return std::nullopt;
	}
	if (pending.epoch != g_sessionEpoch.load(std::memory_order_acquire))
	{
		pending.budget->cancel();
		return std::nullopt;
	}
	const auto result = pending.future.get();
	{
		std::lock_guard<std::mutex> lk(g_lock);
		g_pending.erase(jobId);
	}
	return result;
}

void discard(uint64_t jobId)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (const auto it = g_pending.find(jobId); it != g_pending.end())
	{
		it->second.budget->cancel();
		g_pending.erase(it);
	}
}

void resetSessionState()
{
	{
		std::lock_guard lock(g_lock);
		{
			std::lock_guard circuitLock(g_circuitLock);
			g_sessionEpoch.fetch_add(1, std::memory_order_acq_rel);
			g_requestCodeCircuit.cancelCurrentAttempt();
		}
		for (const auto& [job, pending] : g_pending)
			pending.budget->cancel();
		g_pending.clear();
	}
	{
		std::lock_guard lock(g_codeLock);
		g_codeByDepotGid.clear();
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

bool isGidNotFound(uint64_t gid)
{
	return isGidNotFoundInternal(gid);
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

void markGidNotFound(uint64_t gid)
{
	markGidNotFoundInternal(gid);
}

} // namespace ManifestFetch
