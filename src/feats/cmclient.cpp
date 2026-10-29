// SPDX-License-Identifier: AGPL-3.0-only
//
// See cmclient.hpp for design notes.
//
// Transport overview
// ------------------
//   1. Fetch the public CM websocket server list (no auth) from
//      ISteamDirectory/GetCMListForConnect.
//   2. TLS-connect to wss://<host>:<port>/cmsocket/ using libcurl's
//      CONNECT_ONLY mode (curl 7.81 routes curl_easy_send/recv through
//      the TLS layer transparently — verified empirically; ALPN is
//      disabled so the server speaks plain HTTP/1.1 for the upgrade).
//   3. Perform the RFC 6455 websocket client handshake by hand.
//   4. Frame/deframe binary messages (client frames masked, FIN+0x2).
//   5. Anonymous ClientLogon (EMsg 5514), capture the assigned steamid +
//      client_sessionid from the ClientLogOnResponse (EMsg 751) header.
//   6. Batched ClientPICSProductInfoRequest (EMsg 8903), loop reading
//      responses (EMsg 8904) until response_pending == false, expanding
//      any EMsg.Multi (1) envelopes (gzip-inflated with zlib).

#define CMWIRE_PROTOBUF 1
#include "cmwire.hpp"

#include "cmclient.hpp"

#include "cmlist_cache.hpp"
#include "provision_network.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../cainfo.hpp"

#include "../sdk/protobufs/steammessages_base.pb.h"

#include "base64/base64.hpp"

#include "../utils/curl_api.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace CmClient
{

namespace
{

// EMsg values (canonical, cross-checked against the reference Steam enums).
constexpr uint32_t EMSG_MULTI                       = 1;
constexpr uint32_t EMSG_CLIENT_LOGON                = 5514;
constexpr uint32_t EMSG_CLIENT_LOGON_RESPONSE       = 751;
constexpr uint32_t EMSG_PICS_PRODUCT_INFO_REQUEST   = 8903;
constexpr uint32_t EMSG_PICS_PRODUCT_INFO_RESPONSE  = 8904;

// Anonymous-user steamid: universe = Public(1), account type =
// AnonUser(10).  Layout: type << 52, universe << 56.
constexpr uint64_t kAnonSteamId =
	(static_cast<uint64_t>(1) << 56) | (static_cast<uint64_t>(10) << 52);

// Overall wall-clock budget for the whole exchange.  Past this we bail
// and the caller falls back to steamcmd.net.
constexpr int kTotalTimeoutSecs = 15;
constexpr long long kTotalTimeoutMs = kTotalTimeoutSecs * 1000LL;

class BatchBudget
{
public:
	using Clock = std::chrono::steady_clock;

	BatchBudget() : m_started(Clock::now()) {}

	long remainingMilliseconds(long operationCapMs) const
	{
		const long long elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        Clock::now() - m_started).count();
		if (elapsed >= kTotalTimeoutMs || operationCapMs <= 0) return 0;
		return static_cast<long>(std::min<long long>(
		    kTotalTimeoutMs - std::max(0LL, elapsed),
		    static_cast<long long>(operationCapMs)));
	}

	bool expired() const { return remainingMilliseconds(kTotalTimeoutMs) == 0; }
	Clock::time_point deadline() const
	{
		return m_started + std::chrono::milliseconds(kTotalTimeoutMs);
	}

private:
	Clock::time_point m_started;
};


std::size_t writeCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
}

// Plain HTTPS GET (one-shot, not CONNECT_ONLY) for the CM list.
AppInfoProvision::NetworkFailure httpsGet(const std::string& url,
                                          std::string& body,
                                          const BatchBudget& budget)
{
	if (!curlApi()) return AppInfoProvision::NetworkFailure::Provider;
	const long timeoutMs = budget.remainingMilliseconds(8000);
	if (timeoutMs == 0) return AppInfoProvision::NetworkFailure::Provider;
	CURL* c = curlApi()->easy_init();
	if (!c) return AppInfoProvision::NetworkFailure::Provider;
	body.clear();
	curlApi()->easy_setopt(c, CURLOPT_URL, url.c_str());
	curlApi()->easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curlApi()->easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
	curlApi()->easy_setopt(c, CURLOPT_WRITEDATA, &body);
	curlApi()->easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
	curlApi()->easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeoutMs, 5000L));
	curlApi()->easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curlApi()->easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-CmClient/0.1");
	// Pin the system trust store (see cainfo.hpp): without it the libcurl
	// Steam loads fails CA verification on SteamOS/Arch and this GET returns
	// nothing -> "empty CM list". No-op when no bundle is found.
	if (const char* f = ca::bundleFile()) curlApi()->easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  curlApi()->easy_setopt(c, CURLOPT_CAPATH, d);
	const CURLcode rc = curlApi()->easy_perform(c);
	long status = 0;
	curlApi()->easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curlApi()->easy_cleanup(c);
	if (rc != CURLE_OK) return AppInfoProvision::classifyCurlFailure(rc);
	if (status != 200) return AppInfoProvision::classifyHttpFailure(status);
	return body.empty() ? AppInfoProvision::NetworkFailure::Provider
	                    : AppInfoProvision::NetworkFailure::None;
}

// ---------------------------------------------------------------------------
// CM server list (public, no auth).  Endpoints are picked out of the JSON
// with a plain string scan.
// ---------------------------------------------------------------------------

struct Endpoint { std::string host; int port; };

// Parse "endpoint":"host:port" entries out of the GetCMListForConnect
// JSON (or our cached copy, which stores one host:port per line).
void parseEndpointsFromJson(const std::string& body, std::vector<Endpoint>& out)
{
	const std::string needle = "\"endpoint\":\"";
	size_t pos = 0;
	while ((pos = body.find(needle, pos)) != std::string::npos)
	{
		pos += needle.size();
		const size_t end = body.find('"', pos);
		if (end == std::string::npos) break;
		const std::string ep = body.substr(pos, end - pos);
		pos = end + 1;
		const auto colon = ep.rfind(':');
		if (colon == std::string::npos) continue;
		Endpoint e;
		e.host = ep.substr(0, colon);
		try { e.port = std::stoi(ep.substr(colon + 1)); }
		catch (...) { continue; }
		if (!e.host.empty() && e.port > 0) out.push_back(std::move(e));
	}
}

// On-disk CM-list cache path (alongside the provisioning picsbuffers).
std::string cmListCachePath()
{
	const std::string dir = g_config.getDir() + "/cache";
	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
	return dir + "/cmlist.txt";
}

// Freshness window for the CM-list cache (seconds).  The list is stable
// bootstrap infra, so a long TTL is safe; a stale entry self-heals
// because CmClient re-fetches when every cached endpoint fails to
// connect.  Override via SLSSTEAM_CMLIST_TTL (0 disables).
long long cmListTtlSecs()
{
	if (const char* ov = std::getenv("SLSSTEAM_CMLIST_TTL"); ov && *ov)
	{
		try { return std::stoll(ov); } catch (...) {}
	}
	return 24 * 3600; // 1 day
}

bool readCachedCmList(std::vector<Endpoint>& out)
{
	const auto path = cmListCachePath();
	struct stat st{};
	if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) return false;
	const long long now = static_cast<long long>(std::time(nullptr));
	if (!cache::isCmListReusable(true, static_cast<long long>(st.st_mtime),
	                             now, cmListTtlSecs()))
		return false;
	std::ifstream ifs(path);
	if (!ifs.is_open()) return false;
	std::string line;
	while (std::getline(ifs, line))
	{
		const auto colon = line.rfind(':');
		if (colon == std::string::npos) continue;
		Endpoint e;
		e.host = line.substr(0, colon);
		try { e.port = std::stoi(line.substr(colon + 1)); }
		catch (...) { continue; }
		if (!e.host.empty() && e.port > 0) out.push_back(std::move(e));
	}
	return !out.empty();
}

void writeCachedCmList(const std::vector<Endpoint>& eps)
{
	if (eps.empty()) return;
	const auto path = cmListCachePath();
	std::ofstream ofs(path, std::ios::trunc);
	if (!ofs.is_open()) return;
	for (const auto& e : eps) ofs << e.host << ':' << e.port << '\n';
}

std::vector<Endpoint> fetchCmList(AppInfoProvision::NetworkFailure& failure,
                                  const BatchBudget& budget)
{
	failure = AppInfoProvision::NetworkFailure::None;
	// Reuse a fresh on-disk cache first — skips the ~400ms HTTP GET on
	// cold boots.  A stale/missing cache falls through to the live fetch.
	{
		std::vector<Endpoint> cached;
		if (readCachedCmList(cached))
		{
			g_pLog->info("CmClient: reusing cached CM list (%zu endpoints)\n",
			             cached.size());
			return cached;
		}
	}

	std::vector<Endpoint> out;
	const std::string url =
		"https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
		"?cellid=0&cmtype=websockets&format=json";
	std::string body;
	failure = httpsGet(url, body, budget);
	if (failure != AppInfoProvision::NetworkFailure::None) return out;

	parseEndpointsFromJson(body, out);
	writeCachedCmList(out);
	return out;
}

// ---------------------------------------------------------------------------
// gzip inflate for CMsgMulti with the zlib already in the process, resolved at
// run time like libcurl (the builder has no zlib headers). No child process:
// this runs on Steam threads, the preinit provisioning pass included.
// `expected` is size_unzipped for a sanity bound.
// ---------------------------------------------------------------------------

// zlib's z_stream; its layout is part of zlib's stable ABI.
struct ZStream
{
	const unsigned char* nextIn;
	unsigned availIn;
	unsigned long totalIn;
	unsigned char* nextOut;
	unsigned availOut;
	unsigned long totalOut;
	const char* msg;
	void* state;
	void* zalloc;
	void* zfree;
	void* opaque;
	int dataType;
	unsigned long adler;
	unsigned long reserved;
};

struct ZlibApi
{
	int (*inflateInit2)(ZStream*, int, const char*, int);
	int (*inflate)(ZStream*, int);
	int (*inflateEnd)(ZStream*);
};

const ZlibApi* zlibApi()
{
	static const ZlibApi* const api = []() -> const ZlibApi* {
		void* lib = dlopen("libz.so.1", RTLD_NOLOAD | RTLD_LAZY);
		if (!lib) lib = dlopen("libz.so.1", RTLD_LAZY);
		if (!lib) return nullptr;
		static ZlibApi resolved {};
		resolved.inflateInit2 = reinterpret_cast<decltype(resolved.inflateInit2)>(
			dlsym(lib, "inflateInit2_"));
		resolved.inflate = reinterpret_cast<decltype(resolved.inflate)>(dlsym(lib, "inflate"));
		resolved.inflateEnd = reinterpret_cast<decltype(resolved.inflateEnd)>(
			dlsym(lib, "inflateEnd"));
		return resolved.inflateInit2 && resolved.inflate && resolved.inflateEnd
			? &resolved : nullptr;
	}();
	return api;
}

bool gzipInflate(const std::string& in, uint32_t expected, std::string& out)
{
	const ZlibApi* const z = zlibApi();
	if (!z || in.empty()) return false;
	// Cap the inflate so a malicious/oversized stream can't exhaust memory.
	const size_t cap = expected ? (static_cast<size_t>(expected) + 64)
	                            : (64u * 1024u * 1024u);
	constexpr int kGzipWindow = 15 + 16;
	constexpr int kOk = 0, kStreamEnd = 1, kNoFlush = 0;
	ZStream stream {};
	stream.nextIn = reinterpret_cast<const unsigned char*>(in.data());
	stream.availIn = static_cast<unsigned>(in.size());
	if (z->inflateInit2(&stream, kGzipWindow, "1.2.11", static_cast<int>(sizeof(ZStream))) != kOk)
		return false;
	out.clear();
	char buf[16384];
	int rc = kOk;
	while (rc == kOk)
	{
		stream.nextOut = reinterpret_cast<unsigned char*>(buf);
		stream.availOut = sizeof(buf);
		rc = z->inflate(&stream, kNoFlush);
		const size_t got = sizeof(buf) - stream.availOut;
		if (out.size() + got > cap) { rc = -1; break; }
		out.append(buf, got);
	}
	z->inflateEnd(&stream);
	if (rc != kStreamEnd) { out.clear(); return false; }
	return !out.empty();
}

// ---------------------------------------------------------------------------
// RFC 6455 websocket framing (client side).
// ---------------------------------------------------------------------------

// A small synchronous TLS socket wrapper around curl CONNECT_ONLY.
class TlsSocket
{
public:
	explicit TlsSocket(const BatchBudget& budget)
	    : m_budget(budget), m_deadline(budget.deadline()) {}
	~TlsSocket() { if (m_c) curlApi()->easy_cleanup(m_c); }

	bool connect(const Endpoint& ep)
	{
		const long timeoutMs = m_budget.remainingMilliseconds(kTotalTimeoutMs);
		if (timeoutMs == 0) return false;
		m_c = curlApi()->easy_init();
		if (!m_c) return false;
		// Use an https URL so curl performs the TLS handshake; the path
		// is irrelevant in CONNECT_ONLY mode.
		const std::string url = "https://" + ep.host + ":" +
		                        std::to_string(ep.port) + "/";
		curlApi()->easy_setopt(m_c, CURLOPT_URL, url.c_str());
		curlApi()->easy_setopt(m_c, CURLOPT_CONNECT_ONLY, 1L);
		// Speak plain HTTP/1.1 for the websocket upgrade — no h2.
		curlApi()->easy_setopt(m_c, CURLOPT_SSL_ENABLE_ALPN, 0L);
		curlApi()->easy_setopt(m_c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeoutMs, 6000L));
		curlApi()->easy_setopt(m_c, CURLOPT_TIMEOUT_MS, timeoutMs);
		curlApi()->easy_setopt(m_c, CURLOPT_NOSIGNAL, 1L);
		// Pin the system trust store so the wss:// TLS handshake verifies on
		// SteamOS/Arch (see cainfo.hpp); no-op when no bundle is found.
		if (const char* f = ca::bundleFile()) curlApi()->easy_setopt(m_c, CURLOPT_CAINFO, f);
		if (const char* d = ca::bundleDir())  curlApi()->easy_setopt(m_c, CURLOPT_CAPATH, d);
		const CURLcode rc = curlApi()->easy_perform(m_c);
		return rc == CURLE_OK;
	}

	// Send all bytes, retrying on CURLE_AGAIN within the deadline.
	bool sendAll(const std::string& data)
	{
		size_t total = 0;
		while (total < data.size())
		{
			if (expired()) return false;
			size_t n = 0;
			const CURLcode rc = curlApi()->easy_send(m_c, data.data() + total,
			                           data.size() - total, &n);
			if (rc == CURLE_OK) { total += n; continue; }
			if (rc == CURLE_AGAIN) { usleep(20 * 1000); continue; }
			return false;
		}
		return true;
	}

	// Receive at least one chunk into the internal buffer.  Returns
	// false on a hard error or deadline; true if some bytes arrived (or
	// CURLE_AGAIN was retried — caller re-checks its parse condition).
	bool recvSome()
	{
		char buf[16384];
		size_t n = 0;
		const CURLcode rc = curlApi()->easy_recv(m_c, buf, sizeof(buf), &n);
		if (rc == CURLE_OK) { m_rx.append(buf, n); return n > 0; }
		if (rc == CURLE_AGAIN) { usleep(20 * 1000); return true; }
		return false;
	}

	std::string& rx() { return m_rx; }

	bool expired() const
	{
		return std::chrono::steady_clock::now() >= m_deadline;
	}

private:
	CURL* m_c = nullptr;
	std::string m_rx;
	const BatchBudget& m_budget;
	std::chrono::steady_clock::time_point m_deadline;
};

std::string randomKeyB64()
{
	std::random_device rd;
	std::string raw(16, '\0');
	for (auto& ch : raw) ch = static_cast<char>(rd() & 0xFF);
	return base64::to_base64(raw);
}

// Perform the HTTP/1.1 websocket upgrade.  Reads until the end of the
// response headers; leaves any trailing bytes in the socket rx buffer.
bool wsHandshake(TlsSocket& sock, const Endpoint& ep)
{
	const std::string key = randomKeyB64();
	std::string req;
	req += "GET /cmsocket/ HTTP/1.1\r\n";
	req += "Host: " + ep.host + ":" + std::to_string(ep.port) + "\r\n";
	req += "Upgrade: websocket\r\n";
	req += "Connection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + key + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";
	req += "\r\n";
	if (!sock.sendAll(req)) return false;

	while (sock.rx().find("\r\n\r\n") == std::string::npos)
	{
		if (sock.expired()) return false;
		if (!sock.recvSome()) return false;
	}
	const auto hdrEnd = sock.rx().find("\r\n\r\n");
	const std::string headers = sock.rx().substr(0, hdrEnd);
	// Status line must be 101.
	if (headers.compare(0, 12, "HTTP/1.1 101") != 0 &&
	    headers.find(" 101 ") == std::string::npos)
	{
		g_pLog->info("CmClient: ws upgrade rejected: %.40s\n", headers.c_str());
		return false;
	}
	// Drop the consumed headers; keep any framed bytes that followed.
	sock.rx().erase(0, hdrEnd + 4);
	return true;
}

// Build a masked client websocket binary frame around `payload`.
std::string wsFrame(const std::string& payload)
{
	std::string f;
	f.push_back(static_cast<char>(0x82)); // FIN + binary opcode
	const size_t len = payload.size();
	uint8_t maskFlag = 0x80;
	if (len <= 125)
	{
		f.push_back(static_cast<char>(maskFlag | len));
	}
	else if (len <= 0xFFFF)
	{
		f.push_back(static_cast<char>(maskFlag | 126));
		f.push_back(static_cast<char>((len >> 8) & 0xFF));
		f.push_back(static_cast<char>(len & 0xFF));
	}
	else
	{
		f.push_back(static_cast<char>(maskFlag | 127));
		for (int i = 7; i >= 0; --i)
			f.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
	}
	uint8_t mask[4];
	std::random_device rd;
	for (auto& m : mask) m = static_cast<uint8_t>(rd() & 0xFF);
	f.append(reinterpret_cast<char*>(mask), 4);
	std::string masked = payload;
	for (size_t i = 0; i < masked.size(); ++i)
		masked[i] = static_cast<char>(masked[i] ^ mask[i % 4]);
	f += masked;
	return f;
}

// Try to extract ONE complete websocket frame's payload from the front
// of `buf`.  On success removes the frame from `buf`, sets `payload` and
// `opcode`, returns true.  Returns false if a full frame isn't buffered
// yet (or on a protocol error, signalled via `error`).  Fragmented
// messages are not reassembled: they fail the session, and the next CM is
// asked for what is still missing.
bool wsTryReadFrame(std::string& buf, std::string& payload,
                    uint8_t& opcode, bool& error)
{
	error = false;
	if (buf.size() < 2) return false;
	const uint8_t b0 = static_cast<uint8_t>(buf[0]);
	const uint8_t b1 = static_cast<uint8_t>(buf[1]);
	opcode = b0 & 0x0F;
	if ((b0 & 0x80) == 0 || opcode == 0x0) { error = true; return false; }
	const bool masked = (b1 & 0x80) != 0; // server frames are NOT masked
	uint64_t len = b1 & 0x7F;
	size_t pos = 2;
	if (len == 126)
	{
		if (buf.size() < pos + 2) return false;
		len = (static_cast<uint8_t>(buf[pos]) << 8) |
		      static_cast<uint8_t>(buf[pos + 1]);
		pos += 2;
	}
	else if (len == 127)
	{
		if (buf.size() < pos + 8) return false;
		len = 0;
		for (int i = 0; i < 8; ++i)
			len = (len << 8) | static_cast<uint8_t>(buf[pos + i]);
		pos += 8;
	}
	if (masked) pos += 4; // shouldn't happen from server; skip if present
	// The frame and its copy must fit a 32-bit process next to Steam.
	if (len > (32u * 1024u * 1024u)) { error = true; return false; }
	if (buf.size() < pos + len) return false;
	payload = buf.substr(pos, len);
	buf.erase(0, pos + len);
	return true;
}

// ---------------------------------------------------------------------------
// CM session: logon + batched PICS.
// ---------------------------------------------------------------------------

struct Session
{
	uint64_t steamid   = kAnonSteamId;
	int32_t  sessionid = 0;
	bool     loggedOn  = false;
};

// Build a framed CM packet for `emsg` with the given protobuf body, using
// the current session identity in the header.
std::string buildCmPacket(uint32_t emsg, const std::string& body,
                          const Session& s)
{
	CMsgProtoBufHeader hdr;
	hdr.set_steamid(s.steamid);
	hdr.set_client_sessionid(s.sessionid);
	const std::string hdrBytes = hdr.SerializeAsString();
	return CmWire::packPacket(emsg, hdrBytes, body);
}

// Dispatch one decoded CM packet.  Recurses into Multi.  Updates `out`
// for PICS apps, `session` on logon response, and sets `picsDone` when a
// PICS response with response_pending==false arrives.  Returns false for
// a PICS response it cannot read, which ends the session.
bool handlePacket(const std::string& pkt, Session& session,
                  std::unordered_map<uint32_t, std::string>& out,
                  std::unordered_map<uint32_t, uint32_t>* changes,
                  bool& gotLogon, bool& picsDone)
{
	uint32_t emsg = 0;
	std::string hdrBytes, body;
	if (!CmWire::parsePacket(pkt, emsg, hdrBytes, body)) return true;

	if (emsg == EMSG_MULTI)
	{
		CMsgMulti multi;
		if (!multi.ParseFromString(body)) return true;
		std::string payload = multi.message_body();
		if (multi.size_unzipped() > 0)
		{
			std::string inflated;
			if (!gzipInflate(payload, multi.size_unzipped(), inflated)) return true;
			payload.swap(inflated);
		}
		for (const auto& inner : CmWire::expandMultiBody(payload))
			if (!handlePacket(inner, session, out, changes, gotLogon, picsDone))
				return false;
		return true;
	}

	// Capture the assigned session identity from the response header.
	if (emsg == EMSG_CLIENT_LOGON_RESPONSE)
	{
		CMsgProtoBufHeader hdr;
		if (hdr.ParseFromString(hdrBytes))
		{
			session.steamid   = hdr.steamid();
			session.sessionid = hdr.client_sessionid();
		}
		CMsgClientLogonResponse resp;
		if (resp.ParseFromString(body))
		{
			gotLogon = (resp.eresult() == 1); // k_EResultOK
			session.loggedOn = gotLogon;
		}
		return true;
	}

	if (emsg == EMSG_PICS_PRODUCT_INFO_RESPONSE)
	{
		bool malformed = false;
		const bool pending = CmWire::parsePicsResponse(body, out, changes, &malformed);
		if (malformed) return false;
		if (!pending) picsDone = true;
	}
	return true;
}

// Read+dispatch frames until `predicate` returns true or the deadline
// hits.  Returns false on transport error / timeout.
template <typename Pred>
bool pumpUntil(TlsSocket& sock, Session& session,
               std::unordered_map<uint32_t, std::string>& out,
               std::unordered_map<uint32_t, uint32_t>* changes,
               bool& gotLogon, bool& picsDone, Pred predicate)
{
	for (;;)
	{
		// Drain any whole frames already buffered.
		for (;;)
		{
			std::string payload; uint8_t opcode = 0; bool err = false;
			if (!wsTryReadFrame(sock.rx(), payload, opcode, err))
			{
				if (err) return false;
				break;
			}
			if (opcode == 0x8) return predicate();    // close: done if answered
			if (opcode == 0x9 || opcode == 0xA) continue; // ping/pong: ignore
			if (opcode == 0x2 &&
			    !handlePacket(payload, session, out, changes, gotLogon, picsDone))
				return false;
		}
		if (predicate()) return true;
		if (sock.expired()) return false;
		if (!sock.recvSome()) return false;
	}
}

// Logs on and asks for every app in `pending` in one request; the CM streams
// the answer. Apps already in `out` stay there if the session drops. True once
// the CM has answered the whole request, which then empties `pending`.
bool runSession(const Endpoint& ep, std::vector<uint32_t>& pending,
                std::unordered_map<uint32_t, std::string>& out,
                std::unordered_map<uint32_t, uint32_t>* changes,
                const BatchBudget& budget)
{
	TlsSocket sock(budget);
	if (!sock.connect(ep)) return false;
	if (!wsHandshake(sock, ep)) return false;

	Session session;
	bool gotLogon = false, picsDone = false;

	// Anonymous logon.
	const std::string logonBody = CmWire::buildAnonLogon();
	if (!sock.sendAll(wsFrame(buildCmPacket(EMSG_CLIENT_LOGON, logonBody, session))))
		return false;
	if (!pumpUntil(sock, session, out, changes, gotLogon, picsDone,
	               [&] { return gotLogon; }))
		return false;
	if (!gotLogon) return false;

	if (!sock.sendAll(wsFrame(buildCmPacket(EMSG_PICS_PRODUCT_INFO_REQUEST,
	                                        CmWire::buildPicsRequest(pending),
	                                        session))))
		return false;
	if (!pumpUntil(sock, session, out, changes, gotLogon, picsDone,
	               [&] { return picsDone; }))
		return false;
	pending.clear();
	return true;
}

} // namespace

bool prepareForThreadedFetch()
{
	(void)zlibApi();
	return curlApi() != nullptr;
}

FetchResult fetchProductInfoDetailed(
    const std::vector<uint32_t>& appids,
    std::unordered_map<uint32_t, std::string>& out,
    std::unordered_map<uint32_t, uint32_t>* changesOut)
{
	if (appids.empty()) return FetchResult::Failed;
	if (!curlApi())
	{
		g_pLog->info("CmClient: libcurl unavailable, falling back\n");
		return FetchResult::Failed;
	}
	const BatchBudget budget;

	AppInfoProvision::NetworkFailure listFailure =
	    AppInfoProvision::NetworkFailure::None;
	auto cms = fetchCmList(listFailure, budget);
	if (cms.empty())
	{
		g_pLog->info("CmClient: empty CM list, falling back\n");
		return listFailure == AppInfoProvision::NetworkFailure::Connectivity
		    ? FetchResult::NetworkUnavailable
		    : FetchResult::Failed;
	}

	// Apps not answered yet. A later CM is asked only for these, and what
	// arrived stays in `out` when no CM serves the rest.
	std::vector<uint32_t> pending = appids;
	const auto dropReceived = [&] {
		pending.erase(std::remove_if(pending.begin(), pending.end(),
		                             [&](uint32_t id) { return out.count(id) != 0; }),
		              pending.end());
	};
	// Try a few CMs before giving up; a single edge may refuse/drop.
	// If they ALL fail, the cached list may be stale — drop it and refetch
	// a fresh one once before conceding to the steamcmd fallback.
	for (int round = 0; round < 2; ++round)
	{
		const size_t maxTries = std::min<size_t>(cms.size(), 4);
		for (size_t i = 0; i < maxTries; ++i)
		{
			if (budget.expired()) break;
			dropReceived();
			if (pending.empty()) break;
			const auto& ep = cms[i];
			g_pLog->info("CmClient: trying CM %s:%d (%zu apps)\n",
			             ep.host.c_str(), ep.port, pending.size());
			if (runSession(ep, pending, out, changesOut, budget))
			{
				g_pLog->info("CmClient: fetched %zu/%zu apps via %s\n",
				             out.size(), appids.size(), ep.host.c_str());
				return FetchResult::Success;
			}
		}
		// All cached endpoints failed on the first round: invalidate the
		// on-disk cache and refetch a fresh list for one more round.
		if (round == 0)
		{
			if (budget.expired() || pending.empty()) break;
			std::error_code ec;
			std::filesystem::remove(cmListCachePath(), ec);
			std::vector<Endpoint> fresh;
			const std::string url =
				"https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
				"?cellid=0&cmtype=websockets&format=json";
			std::string body;
			const auto freshFailure = httpsGet(url, body, budget);
			if (freshFailure == AppInfoProvision::NetworkFailure::None)
			{
				parseEndpointsFromJson(body, fresh);
				writeCachedCmList(fresh);
			}
			if (fresh.empty())
			{
				dropReceived();
				if (freshFailure == AppInfoProvision::NetworkFailure::Connectivity &&
				    pending.size() == appids.size())
				{
					g_pLog->info("CmClient: CM directory unavailable; network appears offline\n");
					return FetchResult::NetworkUnavailable;
				}
				break;
			}
			g_pLog->info("CmClient: cached CMs failed, retrying with fresh list (%zu)\n",
			             fresh.size());
			cms.swap(fresh);
		}
	}
	dropReceived();
	if (pending.size() < appids.size())
	{
		g_pLog->info("CmClient: fetched %zu/%zu apps; %zu unanswered fall back\n",
		             out.size(), appids.size(), pending.size());
		return FetchResult::Success;
	}
	g_pLog->info("CmClient: all CM attempts failed, falling back\n");
	return FetchResult::Failed;
}

} // namespace CmClient
