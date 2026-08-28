
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../feats/manifeststore.hpp"
#include "../log.hpp"
#include "../cainfo.hpp"
#include "boundedexecutor.hpp"
#include "contentserverdirectory.hpp"
#include "manifest_zip.hpp"

#include <curl/curl.h>

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

bool beginProviderAttempt()
{
	std::lock_guard<std::mutex> lock(g_circuitLock);
	return g_requestCodeCircuit.beginAttempt(steadyNowMs());
}

void finishProviderAttempt(bool success, bool rateLimited,
	                         bool transportFailure)
{
	bool wasOffline = false;
	bool nowOffline = false;
	{
		std::lock_guard<std::mutex> lock(g_circuitLock);
		wasOffline = g_requestCodeCircuit.open();
		g_requestCodeCircuit.finishAttempt(
		    steadyNowMs(), success, rateLimited, transportFailure);
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

const std::vector<std::string>& providerChain()
{
	static const std::vector<std::string> chain = {
		"http://gmrc.wudrm.com/manifest/{gid}",
	};
	return chain;
}


std::mutex g_lock;
std::map<uint64_t, std::shared_future<std::optional<uint64_t>>> g_pending;

// Cache of resolved manifest request codes keyed by gid.  The same gid
// is resolved twice in a normal install: once by the blob fetch inside
// BYldRequestDepotManifest (to download the .manifest), and again when
// Steam's GetManifestRequestCode job response arrives.  Caching by gid
// lets the second lookup return instantly instead of racing a fresh
// HTTP round-trip — which is what caused the first install attempt to
// fail with "NO INTERNET CONNECTION" (the job response arrived before
// the per-job async resolve finished) and only succeed on retry.
std::mutex g_codeLock;
std::map<uint64_t, uint64_t> g_codeByGid;

void cacheCode(uint64_t gid, uint64_t code)
{
	if (!gid || !code) return;
	std::lock_guard<std::mutex> lk(g_codeLock);
	g_codeByGid[gid] = code;
}

std::optional<uint64_t> cachedCode(uint64_t gid)
{
	std::lock_guard<std::mutex> lk(g_codeLock);
	auto it = g_codeByGid.find(gid);
	if (it == g_codeByGid.end()) return std::nullopt;
	return it->second;
}

// Drop a cached request-code so the next runOnce() for this gid re-resolves
// a fresh one from the provider instead of reusing the expired one.  Codes
// carry a ~5-min CDN TTL; once the CDN starts answering 401, the cached code
// is dead and must be evicted or every retry repeats the 401.
void invalidateCode(uint64_t gid)
{
	std::lock_guard<std::mutex> lk(g_codeLock);
	g_codeByGid.erase(gid);
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

std::string expandTemplate(std::string_view tmpl,
                           uint64_t gid, uint32_t appId, uint32_t depotId)
{
	std::string out;
	out.reserve(tmpl.size() + 32);
	for (std::size_t i = 0; i < tmpl.size();)
	{
		if (tmpl[i] != '{') { out.push_back(tmpl[i++]); continue; }
		auto end = tmpl.find('}', i + 1);
		if (end == std::string_view::npos) { out.push_back(tmpl[i++]); continue; }
		auto tag = tmpl.substr(i + 1, end - i - 1);
		if (tag == "gid")          out += std::to_string(gid);
		else if (tag == "appid")   out += std::to_string(appId);
		else if (tag == "depotid") out += std::to_string(depotId);
		else                       out.append(tmpl.substr(i, end - i + 1));
		i = end + 1;
	}
	return out;
}

struct HttpResponse
{
	long status         = 0;
	std::string body;
	bool networkError   = false;
	std::string diagnostic;
};

std::size_t curlWriteCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
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

static bool load_curl() {
	if (p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform
	    && p_curl_easy_cleanup && p_curl_slist_append
	    && p_curl_slist_free_all)
	{
		return true;
	}

	void* handle = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!handle) handle = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!handle) handle = RTLD_DEFAULT;

	p_curl_easy_init = (curl_easy_init_t)dlsym(handle, "curl_easy_init");
	p_curl_easy_setopt = (curl_easy_setopt_t)dlsym(handle, "curl_easy_setopt");
	p_curl_easy_perform = (curl_easy_perform_t)dlsym(handle, "curl_easy_perform");
	p_curl_easy_cleanup = (curl_easy_cleanup_t)dlsym(handle, "curl_easy_cleanup");
	p_curl_easy_getinfo = (curl_easy_getinfo_t)dlsym(handle, "curl_easy_getinfo");
	p_curl_easy_strerror = (curl_easy_strerror_t)dlsym(handle, "curl_easy_strerror");
	p_curl_slist_append = (curl_slist_append_t)dlsym(handle, "curl_slist_append");
	p_curl_slist_free_all = (curl_slist_free_all_t)dlsym(handle, "curl_slist_free_all");

	return p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform
	       && p_curl_easy_cleanup && p_curl_slist_append
	       && p_curl_slist_free_all;
}

int curlBudgetProgress(void* userdata,
                       curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const auto* budget = static_cast<const JobBudget*>(userdata);
	return budget != nullptr && budget->shouldStop() ? 1 : 0;
}

HttpResponse httpGet(const std::string& url, const JobBudget* budget = nullptr,
                     std::string_view hostHeader = {})
{
	HttpResponse r;
	if (budget != nullptr && budget->shouldStop())
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
	p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
	if (budget == nullptr)
	{
		p_curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
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
		if (remainingMs > 10000) remainingMs = 10000;
		const long timeoutMs = static_cast<long>(remainingMs < 1 ? 1 : remainingMs);
		const long connectMs = timeoutMs < 5000 ? timeoutMs : 5000;
		p_curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
		p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, connectMs);
		p_curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		p_curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlBudgetProgress);
		p_curl_easy_setopt(c, CURLOPT_XFERINFODATA, budget);
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
                                const std::shared_ptr<JobBudget>& budget = {})
{
	if (budget && budget->shouldStop()) return std::nullopt;

	// Fast path: a previous resolve (e.g. the blob fetch in
	// BYldRequestDepotManifest) already learned this gid's request
	// code.  Reuse it so we don't race a fresh HTTP round-trip when
	// Steam's GetManifestRequestCode job response arrives.
	if (auto c = cachedCode(gid))
	{
		return c;
	}

	// An open circuit admits one actual queued gid after the cooldown. This is
	// both the recovery probe and useful work; a dead provider's gid=0 404 can
	// never reset health falsely.
	if (!beginProviderAttempt())
	{
		g_pLog->debug(
		    "ManifestFetch: gid=%llu skipped during request-code cooldown\n",
		    static_cast<unsigned long long>(gid));
		return std::nullopt;
	}

	const auto& chain = providerChain();
	if (chain.empty())
	{
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
		const auto& tmpl = chain[i];
		if (tmpl.empty()) continue;
		const auto url = expandTemplate(tmpl, gid, appId, depotId);
		g_pLog->info("ManifestFetch: gid=%llu provider %zu/%zu GET %s\n",
		             static_cast<unsigned long long>(gid),
		             i + 1, chain.size(), url.c_str());

		const auto resp = httpGet(url, budget.get());
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
			g_pLog->info("ManifestFetch: gid=%llu resolved code=%llu via provider %zu\n",
			             static_cast<unsigned long long>(gid),
			             static_cast<unsigned long long>(code),
			             i + 1);
			cacheCode(gid, code);
			finishProviderAttempt(
			    /*success=*/true, /*rateLimited=*/false,
			    /*transportFailure=*/false);
			return code;
		}
		g_pLog->info("ManifestFetch: gid=%llu provider %zu body unparseable (first 64: '%.*s'), trying next\n",
		             static_cast<unsigned long long>(gid),
		             i + 1,
		             static_cast<int>(std::min<std::size_t>(resp.body.size(), 64)),
		             resp.body.c_str());
	}

	g_pLog->info("ManifestFetch: gid=%llu all %zu providers exhausted\n",
	             static_cast<unsigned long long>(gid), chain.size());
	if (isDefinitiveNotFound(outcomes))
		markGidNotFoundInternal(gid);

	finishProviderAttempt(
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

bool writeManifestFile(const std::string& path,
                       const std::vector<unsigned char>& bytes)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
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

bool fetchManifestBlob(uint64_t gid, uint32_t depotId,
                       const std::string& depotcacheDir,
                       const std::shared_ptr<JobBudget>& budget)
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

	auto codeOpt = runOnce(gid, /*appId=*/0, depotId, budget);
	if (!codeOpt)
	{
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu request-code lookup failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		// Log-only: the manifest resilience fallback handles providers-down
		// (see the all-providers-exhausted path above).
		return false;
	}
	uint64_t code = *codeOpt;

	// Ask Valve's content directory for a region/load-ranked list.  cell_id=0
	// lets the service choose for the requester's public IP, matching Steam's
	// normal content path without pinning users to one geographic cell.
	const auto servers = contentServers(budget);
	if (servers.empty()) return false;

	bool retriedWithFreshCode = false;
retry_cdn:
	HttpResponse zipResp;
	bool gotZip = false;
	std::vector<CdnOutcome> outcomes;
	outcomes.reserve(servers.size());
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
		outcomes.push_back({ zipResp.networkError, zipResp.status });
		// info, not warn: warn fires a notify-send popup; a single edge
		// returning 503 (or an expired code returning 401) is expected and
		// we just try the next host.
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu host=%s HTTP=%ld err='%s', trying next CDN\n",
		             depotId, static_cast<unsigned long long>(gid), server.host.c_str(),
		             zipResp.status, zipResp.diagnostic.c_str());
	}
	if (!gotZip)
	{
		// A unanimous 401 across every host is the signature of an expired
		// request-code (codes carry a ~5-min CDN TTL).  This is exactly what
		// the background pre-warm worker hits when it re-stages a DLC depot
		// Steam purged minutes after the base install committed.  Evict the
		// stale code, re-resolve a fresh one, and retry the CDN ONCE.
		if (!retriedWithFreshCode && isExpiredCodeSignature(outcomes))
		{
			retriedWithFreshCode = true;
			invalidateCode(gid);
			g_pLog->info("ManifestFetch: blob depot=%u gid=%llu code expired (all 401), re-resolving\n",
			             depotId, static_cast<unsigned long long>(gid));
			if (auto freshCode = runOnce(gid, /*appId=*/0, depotId, budget))
			{
				code = *freshCode;
				goto retry_cdn;
			}
		}
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu all CDN hosts failed (last HTTP=%ld)\n",
		             depotId, static_cast<unsigned long long>(gid), zipResp.status);
		// Log-only: this is our manifest-blob staging fetch; the resilience
		// fallback (feats/manifestbind.cpp) installs from a local manifest
		// when it fails, so the download proceeds.  A genuine content-CDN
		// outage (chunks unreachable) is surfaced by Steam's own UI.
		return false;
	}

	std::vector<unsigned char> manifest;
	std::string extractDiagnostic;
	if (!ManifestZip::extractSingleFile(zipResp.body, manifest, &extractDiagnostic))
	{
		g_pLog->warn(
			"ManifestFetch: blob depot=%u gid=%llu archive rejected: %s\n",
			depotId, static_cast<unsigned long long>(gid),
			extractDiagnostic.c_str());
		return false;
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
		return false;
	}
	if (!writeManifestFile(tmpOutPath, manifest))
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu manifest write failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return false;
	}

	if (!ManifestStore::publishDownloadedManifest(depotId, gid, tmpOutPath))
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn(
		    "ManifestFetch: blob depot=%u gid=%llu persistent publish failed\n",
		    depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return false;
	}
	unlink(tmpOutPath.c_str());

	g_pLog->info("ManifestFetch: blob depot=%u gid=%llu staged at %s\n",
	             depotId, static_cast<unsigned long long>(gid),
	             targetPath.c_str());
	return true;
}

} // namespace

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
	std::atomic<int> waiters{0};
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

std::shared_ptr<BlobJob> launchOrJoinBlob(uint64_t gid, uint32_t depotId,
                                          const std::string& depotcacheDir,
                                          bool registerWaiter)
{
	const BlobKey key{gid, depotId};
	std::lock_guard<std::mutex> lk(g_blobLock);
	auto it = g_blobInflight.find(key);
	if (it != g_blobInflight.end())
	{
		auto job = it->second;
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
		std::chrono::milliseconds(getTimeoutSec() * 1000));
	if (registerWaiter) detail::registerWaiterLocked(job->waiters);
	g_blobInflight.emplace(key, job);

	const bool accepted = blobExecutor().submit(
	    [gid, depotId, depotcacheDir, completion, job, key]
	    {
	        bool ok = false;
	        try
	        {
	            if (!job->budget->shouldStop())
	            {
	                // Do filesystem restoration and any network fetch on our
	                // executor, never on Steam's PICS/IPC worker.
	                ok = ManifestStore::restoreToDepotcache(depotId, gid)
	                     || fetchManifestBlob(gid, depotId, depotcacheDir,
	                                           job->budget);
	            }
	        }
	        catch (...)
	        {
	            g_pLog->info(
	                "ManifestFetch: blob depot=%u gid=%llu worker failed unexpectedly\n",
	                depotId, static_cast<unsigned long long>(gid));
	        }
	        try { completion->set_value(ok); } catch (...) {}
	        {
	            std::lock_guard<std::mutex> lk(g_blobLock);
	            job->producerActive = false;
	            auto it = g_blobInflight.find(key);
	            if (it != g_blobInflight.end() && it->second == job)
	                g_blobInflight.erase(it);
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

void submitManifestBlob(uint64_t manifestGid, uint32_t /*appId*/, uint32_t depotId)
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
	(void)launchOrJoinBlob(manifestGid, depotId, depotcacheDir,
	                       /*registerWaiter=*/false);
}

bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId, int timeoutSec)
{
	if (timeoutSec <= 0) timeoutSec = getTimeoutSec();
	return awaitManifestBlobFor(
	    manifestGid, depotId, timeoutSec * 1000, /*notifyOnTimeout=*/true);
}

bool awaitManifestBlobFor(uint64_t manifestGid, uint32_t depotId,
                          int timeoutMs, bool notifyOnTimeout)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty()) return false;
	const std::string depotcacheDir = steamRoot + "/depotcache";
	auto job = launchOrJoinBlob(manifestGid, depotId, depotcacheDir,
	                            /*registerWaiter=*/true);
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

bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t depotId)
{
	return awaitManifestBlob(manifestGid, depotId, getTimeoutSec());
}

void submit(uint64_t jobId, uint64_t manifestGid, uint32_t appId, uint32_t depotId)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (g_pending.count(jobId))
	{
		g_pLog->debug("ManifestFetch: duplicate submit for jobId=%llu\n",
		              static_cast<unsigned long long>(jobId));
		return;
	}
	auto fut = std::async(std::launch::async,
	                      [manifestGid, appId, depotId]() -> std::optional<uint64_t>
	                      {
	                          return runOnce(manifestGid, appId, depotId);
	                      });
	g_pending.emplace(jobId, fut.share());
}

std::optional<uint64_t> resolve(uint64_t jobId)
{
	std::shared_future<std::optional<uint64_t>> fut;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		auto it = g_pending.find(jobId);
		if (it == g_pending.end()) return std::nullopt;
		fut = it->second;
		g_pending.erase(it);
	}
	const int budget = getTimeoutSec() > 0 ? getTimeoutSec() : 12;
	if (fut.wait_for(std::chrono::seconds(budget)) != std::future_status::ready)
	{
		g_pLog->info("ManifestFetch: jobId=%llu timed out after %ds\n",
		             static_cast<unsigned long long>(jobId), budget);
		g_pLog->notifyUser(UserMsg::DownloadTimedOut);
		return std::nullopt;
	}
	return fut.get();
}

void discard(uint64_t jobId)
{
	std::lock_guard<std::mutex> lk(g_lock);
	g_pending.erase(jobId);
}

bool areProvidersOffline()
{
	return g_providersOffline.load();
}

bool isGidNotFound(uint64_t gid)
{
	return isGidNotFoundInternal(gid);
}

void markGidNotFound(uint64_t gid)
{
	markGidNotFoundInternal(gid);
}

} // namespace ManifestFetch
