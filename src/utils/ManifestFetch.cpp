
#include "ManifestFetch.hpp"

#include "../config.hpp"
#include "../log.hpp"

#include <curl/curl.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>


namespace ManifestFetch
{

namespace
{

const std::vector<std::string>& providerChain()
{
	static const std::vector<std::string> chain = {
		"http://gmrc.wudrm.com/manifest/{gid}",
		"https://manifest.steam.run/api/manifest/{gid}",
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

static curl_easy_init_t p_curl_easy_init = nullptr;
static curl_easy_setopt_t p_curl_easy_setopt = nullptr;
static curl_easy_perform_t p_curl_easy_perform = nullptr;
static curl_easy_cleanup_t p_curl_easy_cleanup = nullptr;
static curl_easy_getinfo_t p_curl_easy_getinfo = nullptr;
static curl_easy_strerror_t p_curl_easy_strerror = nullptr;

static bool load_curl() {
	if (p_curl_easy_init) return true;

	void* handle = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!handle) handle = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!handle) handle = RTLD_DEFAULT;

	p_curl_easy_init = (curl_easy_init_t)dlsym(handle, "curl_easy_init");
	p_curl_easy_setopt = (curl_easy_setopt_t)dlsym(handle, "curl_easy_setopt");
	p_curl_easy_perform = (curl_easy_perform_t)dlsym(handle, "curl_easy_perform");
	p_curl_easy_cleanup = (curl_easy_cleanup_t)dlsym(handle, "curl_easy_cleanup");
	p_curl_easy_getinfo = (curl_easy_getinfo_t)dlsym(handle, "curl_easy_getinfo");
	p_curl_easy_strerror = (curl_easy_strerror_t)dlsym(handle, "curl_easy_strerror");

	return p_curl_easy_init && p_curl_easy_setopt && p_curl_easy_perform && p_curl_easy_cleanup;
}

HttpResponse httpGet(const std::string& url)
{
    HttpResponse r;
    
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
    p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
    p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    p_curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    // MANDATORY for multi-threaded use: httpGet runs on a ManifestFetch
    // worker thread.  Without CURLOPT_NOSIGNAL, libcurl built with a
    // synchronous resolver implements timeouts via SIGALRM + siglongjmp.
    // That handler is process-wide; if SIGALRM fires while another thread
    // (e.g. Steam's main thread in poll()) is running, the longjmp targets
    // the wrong stack and glibc's __longjmp_chk aborts the whole client.
    // NOSIGNAL switches libcurl to signal-free timeouts.
    p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-ManifestFetch/0.1");
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
    p_curl_easy_cleanup(c);
    return r;
}

std::optional<uint64_t> runOnce(uint64_t gid, uint32_t appId, uint32_t depotId)
{
	// Fast path: a previous resolve (e.g. the blob fetch in
	// BYldRequestDepotManifest) already learned this gid's request
	// code.  Reuse it so we don't race a fresh HTTP round-trip when
	// Steam's GetManifestRequestCode job response arrives.
	if (auto c = cachedCode(gid))
	{
		return c;
	}

	const auto& chain = providerChain();
	if (chain.empty())
	{
		g_pLog->debug("ManifestFetch: gid=%llu skipped, no providers configured\n",
		              static_cast<unsigned long long>(gid));
		return std::nullopt;
	}

	for (std::size_t i = 0; i < chain.size(); ++i)
	{
		const auto& tmpl = chain[i];
		if (tmpl.empty()) continue;
		const auto url = expandTemplate(tmpl, gid, appId, depotId);
		g_pLog->info("ManifestFetch: gid=%llu provider %zu/%zu GET %s\n",
		             static_cast<unsigned long long>(gid),
		             i + 1, chain.size(), url.c_str());

		const auto resp = httpGet(url);
		if (resp.networkError)
		{
			g_pLog->info("ManifestFetch: gid=%llu provider %zu net err '%s', trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.diagnostic.c_str());
			continue;
		}
		if (resp.status != 200)
		{
			g_pLog->info("ManifestFetch: gid=%llu provider %zu HTTP=%ld body_bytes=%zu, trying next\n",
			             static_cast<unsigned long long>(gid),
			             i + 1, resp.status, resp.body.size());
			continue;
		}
		uint64_t code = 0;
		if (parseDigitsOnly(resp.body, &code) || parseJsonDigitField(resp.body, &code))
		{
			g_pLog->info("ManifestFetch: gid=%llu resolved code=%llu via provider %zu\n",
			             static_cast<unsigned long long>(gid),
			             static_cast<unsigned long long>(code),
			             i + 1);
			cacheCode(gid, code);
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

bool fetchManifestBlob(uint64_t gid, uint32_t depotId, const std::string& depotcacheDir)
{
	std::string targetPath = depotcacheDir + "/" + std::to_string(depotId)
	                          + "_" + std::to_string(gid) + ".manifest";
	{
		struct stat st{};
		if (stat(targetPath.c_str(), &st) == 0 && st.st_size > 0)
		{
			g_pLog->debug("ManifestFetch: blob depot=%u gid=%llu already at %s\n",
			              depotId,
			              static_cast<unsigned long long>(gid),
			              targetPath.c_str());
			return true;
		}
	}

	auto codeOpt = runOnce(gid, /*appId=*/0, depotId);
	if (!codeOpt)
	{
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu request-code lookup failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		// Log-only: the manifest resilience fallback handles providers-down
		// (see the all-providers-exhausted path above).
		return false;
	}
	uint64_t code = *codeOpt;

	// Steam's manifest CDN occasionally answers 503 (overloaded edge)
	// for a given host.  Try a handful of CDN hosts before giving up so
	// a transient 503 doesn't surface as "NO INTERNET CONNECTION".
	static const char* kCdnHosts[] = {
		"cache1-gru1.steamcontent.com",
		"cache2-gru1.steamcontent.com",
		"cache4-gru1.steamcontent.com",
		"cache8-gru1.steamcontent.com",
		"cache11-gru1.steamcontent.com",
		"fastly.cdn.steampipe.steamcontent.com",
	};

	bool retriedWithFreshCode = false;
retry_cdn:
	HttpResponse zipResp;
	bool gotZip = false;
	std::vector<CdnOutcome> outcomes;
	outcomes.reserve(sizeof(kCdnHosts) / sizeof(kCdnHosts[0]));
	for (const char* host : kCdnHosts)
	{
		const std::string cdnUrl = std::string("http://") + host + "/depot/"
		                           + std::to_string(depotId) + "/manifest/"
		                           + std::to_string(gid) + "/5/"
		                           + std::to_string(code);
		zipResp = httpGet(cdnUrl);
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
		             depotId, static_cast<unsigned long long>(gid), host,
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
			if (auto freshCode = runOnce(gid, /*appId=*/0, depotId))
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

	char tmpZip[]  = "/tmp/slsteam_mfetch_zip_XXXXXX";
	int tmpZipFd = mkstemp(tmpZip);
	if (tmpZipFd < 0)
	{
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu mkstemp failed\n",
		             depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return false;
	}
	const ssize_t written =
	    write(tmpZipFd, zipResp.body.data(), zipResp.body.size());
	close(tmpZipFd);
	if (written != static_cast<ssize_t>(zipResp.body.size()))
	{
		unlink(tmpZip);
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu zip write short\n",
		             depotId, static_cast<unsigned long long>(gid));
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return false;
	}

	const std::string tmpOutPath = targetPath + ".slsteam_tmp." +
	                               std::to_string(static_cast<unsigned long>(getpid())) + "." +
	                               std::to_string(reinterpret_cast<uintptr_t>(&zipResp));
	const std::string cmd =
	    "unzip -p " + std::string(tmpZip) + " > " + tmpOutPath + " 2>/dev/null";
	const int rc = std::system(cmd.c_str());
	unlink(tmpZip);
	if (rc != 0)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu unzip rc=%d\n",
		             depotId, static_cast<unsigned long long>(gid), rc);
		return false;
	}

	std::ifstream verify(tmpOutPath, std::ios::binary);
	uint32_t magic = 0;
	verify.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	verify.close();
	if (magic != 0x71F617D0u)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu bad magic 0x%x\n",
		             depotId, static_cast<unsigned long long>(gid), magic);
		return false;
	}

	if (rename(tmpOutPath.c_str(), targetPath.c_str()) != 0)
	{
		unlink(tmpOutPath.c_str());
		g_pLog->warn("ManifestFetch: blob depot=%u gid=%llu rename failed errno=%d\n",
		             depotId, static_cast<unsigned long long>(gid), errno);
		g_pLog->notifyUser(UserMsg::LocalStorageError);
		return false;
	}

	g_pLog->info("ManifestFetch: blob depot=%u gid=%llu wrote %s\n",
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

std::mutex g_blobLock;
std::unordered_map<BlobKey, std::shared_future<bool>, BlobKeyHash, BlobKeyEq> g_blobInflight;

std::shared_future<bool> launchOrJoinBlob(uint64_t gid, uint32_t depotId,
                                          const std::string& depotcacheDir)
{
	const BlobKey key{gid, depotId};
	std::lock_guard<std::mutex> lk(g_blobLock);
	auto it = g_blobInflight.find(key);
	if (it != g_blobInflight.end())
	{
		// If the previous job finished successfully AND the manifest is
		// still on disk, return that.  If it finished but failed, OR the
		// file is gone, drop the entry so a fresh re-fetch happens.
		//
		// The on-disk re-check is essential: Steam purges sibling depot
		// manifests from depotcache when it commits a base depot, so a
		// manifest we staged once (e.g. a DLC depot like 238325, staged
		// up-front in the PICS recv handler) can vanish before Steam
		// plans that depot.  Without this check a cached success made
		// BYldRequestDepotManifest's fallback report "staged on disk"
		// while never re-writing the file — Steam then looped forever on
		// "Access Denied / No connection" because the manifest stayed
		// deleted.  Re-checking lets the fallback actually re-stage it so
		// the next planning pass finds it and skips BYld entirely.
		if (it->second.wait_for(std::chrono::seconds(0)) ==
		    std::future_status::ready)
		{
			const std::string targetPath = depotcacheDir + "/" +
			    std::to_string(depotId) + "_" + std::to_string(gid) + ".manifest";
			struct stat st{};
			const bool onDisk =
			    (stat(targetPath.c_str(), &st) == 0 && st.st_size > 0);
			if (it->second.get() && onDisk)
			{
				return it->second;
			}
			g_blobInflight.erase(it);
		}
		else
		{
			return it->second;
		}
	}
	auto fut = std::async(std::launch::async,
	    [gid, depotId, depotcacheDir]() -> bool
	    {
	        return fetchManifestBlob(gid, depotId, depotcacheDir);
	    }).share();
	g_blobInflight.emplace(key, fut);
	return fut;
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
	(void)launchOrJoinBlob(manifestGid, depotId, depotcacheDir);
}

bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId, int timeoutSec)
{
	const auto steamRoot = findSteamRootForBlob();
	if (steamRoot.empty()) return false;
	const std::string depotcacheDir = steamRoot + "/depotcache";
	auto fut = launchOrJoinBlob(manifestGid, depotId, depotcacheDir);
	if (timeoutSec <= 0) timeoutSec = getTimeoutSec();
	if (fut.wait_for(std::chrono::seconds(timeoutSec)) !=
	    std::future_status::ready)
	{
		g_pLog->info("ManifestFetch: blob depot=%u gid=%llu await timed out after %ds\n",
		             depotId, static_cast<unsigned long long>(manifestGid), timeoutSec);
		g_pLog->notifyUser(UserMsg::DownloadTimedOut);
		return false;
	}
	return fut.get();
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

} // namespace ManifestFetch
