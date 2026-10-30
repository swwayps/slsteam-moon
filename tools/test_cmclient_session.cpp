// CmClient::fetchProductInfoDetailed against scripted CMs: a session that
// drops keeps every app the CM already sent, and the next CM gets the rest.
// Compressed answers are read in process, without a gzip program.
// Build: make test-cmclient-session (links tools/fake_libcurl.cpp as libcurl).
#include "fake_libcurl.hpp"

#define CMWIRE_PROTOBUF 1
#include "../src/feats/cmwire.hpp"
#include "../src/feats/cmclient.hpp"
#include "../src/log.hpp"
#include "../src/sdk/protobufs/steammessages_base.pb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

// A real CM splits a large answer over several messages too.
constexpr std::size_t kAppsPerResponse = 100;

// The CM knows no product info for these ids.
bool unknownApp(uint32_t id) { return id % 10 == 0; }

// zlib's stable z_stream ABI; the builder has no zlib headers.
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

std::string gzipped(const std::string& data)
{
	void* lib = dlopen("libz.so.1", RTLD_LAZY);
	if (!lib) return {};
	using Init = int (*)(ZStream*, int, int, int, int, int, const char*, int);
	using Deflate = int (*)(ZStream*, int);
	using End = int (*)(ZStream*);
	const auto init = reinterpret_cast<Init>(dlsym(lib, "deflateInit2_"));
	const auto deflate = reinterpret_cast<Deflate>(dlsym(lib, "deflate"));
	const auto end = reinterpret_cast<End>(dlsym(lib, "deflateEnd"));
	if (!init || !deflate || !end) return {};
	ZStream stream{};
	// Z_DEFAULT_COMPRESSION, Z_DEFLATED, gzip window, default memLevel and strategy.
	if (init(&stream, -1, 8, 15 + 16, 8, 0, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
		return {};
	std::string out(data.size() + 1024, '\0');
	stream.nextIn = reinterpret_cast<const unsigned char*>(data.data());
	stream.availIn = static_cast<unsigned>(data.size());
	stream.nextOut = reinterpret_cast<unsigned char*>(out.data());
	stream.availOut = static_cast<unsigned>(out.size());
	const int rc = deflate(&stream, 4); // Z_FINISH
	out.resize(out.size() - stream.availOut);
	end(&stream);
	return rc == 1 ? out : std::string{}; // Z_STREAM_END
}

struct Handle
{
	std::string url;
	std::size_t (*write)(char*, std::size_t, std::size_t, void*) = nullptr;
	void* writeData = nullptr;
	bool connectOnly = false;
	long redirProtocols = -1;
	std::string host;
	std::string tx, rx;
	bool upgraded = false;
	bool dropped = false;
};

struct ScriptedEndpoint
{
	bool refuse = false;
	int responsesLeft = -1; // product-info messages before every later socket drops
	bool withholdFinal = false; // drop instead of sending the last "not pending" flag
	bool gzip = false; // send each answer gzip-compressed inside a Multi
	bool fragment = false; // send each answer as a frame without FIN plus a continuation
	bool corruptSecond = false; // send an unreadable body as the second answer
	bool closeAfterFinal = false; // send a close frame right after the last answer
	std::vector<std::vector<uint32_t>> requests;
	std::unordered_set<uint32_t> delivered; // apps sent with a buffer
};

class ScriptedCms final : public FakeCurl
{
public:
	std::map<std::string, ScriptedEndpoint> endpoints;
	std::size_t directoryPadding = 0; // spaces inside the CM directory JSON
	std::size_t directoryAccepted = 0; // bytes the write callback kept
	long directoryRedirect = -1; // CURLOPT_REDIR_PROTOCOLS of the directory GET

	CURL* init() override { return reinterpret_cast<CURL*>(new Handle); }

	CURLcode setopt(CURL* c, CURLoption option, long number, void* pointer) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		if (option == CURLOPT_URL) h.url = static_cast<const char*>(pointer);
		else if (option == CURLOPT_WRITEFUNCTION)
			h.write = reinterpret_cast<std::size_t (*)(char*, std::size_t, std::size_t, void*)>(pointer);
		else if (option == CURLOPT_WRITEDATA) h.writeData = pointer;
		else if (option == CURLOPT_CONNECT_ONLY) h.connectOnly = number != 0;
		else if (option == CURLOPT_REDIR_PROTOCOLS) h.redirProtocols = number;
		return CURLE_OK;
	}

	CURLcode perform(CURL* c) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		if (!h.connectOnly)
		{
			// The CM directory.
			std::string body = "{\"response\":{\"serverlist\":[";
			for (const auto& [host, endpoint] : endpoints)
			{
				(void)endpoint;
				if (body.back() == '}') body += ',';
				body += "{\"endpoint\":\"" + host + ":443\"}";
			}
			body += std::string(directoryPadding, ' ') + "]}}";
			directoryRedirect = h.redirProtocols;
			// Delivered in chunks, like libcurl: a short count ends the transfer.
			for (std::size_t at = 0; at < body.size(); at += 65536)
			{
				const std::size_t chunk = std::min<std::size_t>(65536, body.size() - at);
				if (h.write(body.data() + at, 1, chunk, h.writeData) != chunk)
					return CURLE_WRITE_ERROR;
				directoryAccepted += chunk;
			}
			return CURLE_OK;
		}
		const std::size_t start = std::strlen("https://");
		h.host = h.url.substr(start, h.url.rfind(':') - start);
		return endpoints[h.host].refuse ? CURLE_COULDNT_CONNECT : CURLE_OK;
	}

	void cleanup(CURL* c) override { delete reinterpret_cast<Handle*>(c); }

	CURLcode getinfo(CURL*, CURLINFO info, void* out) override
	{
		if (info == CURLINFO_RESPONSE_CODE) *static_cast<long*>(out) = 200;
		return CURLE_OK;
	}

	CURLcode send(CURL* c, const void* data, std::size_t length, std::size_t* sent) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		h.tx.append(static_cast<const char*>(data), length);
		*sent = length;
		if (!h.upgraded)
		{
			const auto end = h.tx.find("\r\n\r\n");
			if (end == std::string::npos) return CURLE_OK;
			h.tx.erase(0, end + 4);
			h.rx += "HTTP/1.1 101 Switching Protocols\r\n\r\n";
			h.upgraded = true;
		}
		std::string packet;
		while (takeClientFrame(h.tx, packet)) answer(h, packet);
		return CURLE_OK;
	}

	CURLcode recv(CURL* c, void* buffer, std::size_t length, std::size_t* received) override
	{
		Handle& h = *reinterpret_cast<Handle*>(c);
		*received = 0;
		if (h.rx.empty()) return h.dropped ? CURLE_RECV_ERROR : CURLE_AGAIN;
		*received = std::min(length, h.rx.size());
		std::memcpy(buffer, h.rx.data(), *received);
		h.rx.erase(0, *received);
		return CURLE_OK;
	}

private:
	static bool takeClientFrame(std::string& tx, std::string& payload)
	{
		if (tx.size() < 2) return false;
		std::uint64_t length = static_cast<std::uint8_t>(tx[1]) & 0x7F;
		std::size_t at = 2;
		if (length == 126 || length == 127)
		{
			const std::size_t bytes = length == 126 ? 2 : 8;
			if (tx.size() < at + bytes) return false;
			length = 0;
			for (std::size_t i = 0; i < bytes; ++i)
				length = (length << 8) | static_cast<std::uint8_t>(tx[at + i]);
			at += bytes;
		}
		if (tx.size() < at + 4 + length) return false;
		const std::string mask = tx.substr(at, 4);
		payload = tx.substr(at + 4, length);
		for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
		tx.erase(0, at + 4 + length);
		return true;
	}

	static std::string frame(std::uint8_t head, const std::string& payload)
	{
		std::string out(1, static_cast<char>(head));
		if (payload.size() <= 125)
			out += static_cast<char>(payload.size());
		else
		{
			out += static_cast<char>(127);
			for (int i = 7; i >= 0; --i)
				out += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (i * 8)) & 0xFF);
		}
		return out + payload;
	}

	static void queue(Handle& h, uint32_t emsg, const std::string& header,
	                  const std::string& body, bool fragment = false)
	{
		const std::string packet = CmWire::packPacket(emsg, header, body);
		if (!fragment)
		{
			h.rx += frame(0x82, packet);
			return;
		}
		const std::size_t half = packet.size() / 2;
		h.rx += frame(0x02, packet.substr(0, half)) + frame(0x80, packet.substr(half));
	}

	void answer(Handle& h, const std::string& packet)
	{
		uint32_t emsg = 0;
		std::string header, body;
		if (!CmWire::parsePacket(packet, emsg, header, body)) return;
		if (emsg == 5514) // ClientLogon
		{
			CMsgProtoBufHeader assigned;
			assigned.set_steamid((1ULL << 56) | (10ULL << 52) | 1);
			assigned.set_client_sessionid(7);
			CMsgClientLogonResponse logon;
			logon.set_eresult(1);
			queue(h, 751, assigned.SerializeAsString(), logon.SerializeAsString());
			return;
		}
		if (emsg != 8903) return; // ClientPICSProductInfoRequest
		CMsgClientPICSProductInfoRequest request;
		if (!request.ParseFromString(body)) return;
		std::vector<uint32_t> ids;
		for (const auto& app : request.apps()) ids.push_back(app.appid());
		ScriptedEndpoint& endpoint = endpoints[h.host];
		endpoint.requests.push_back(ids);
		for (std::size_t at = 0; at < ids.size(); at += kAppsPerResponse)
		{
			if (endpoint.responsesLeft == 0)
			{
				h.dropped = true;
				return;
			}
			if (endpoint.responsesLeft > 0) --endpoint.responsesLeft;
			const std::size_t end = std::min(ids.size(), at + kAppsPerResponse);
			CMsgClientPICSProductInfoResponse response;
			for (std::size_t i = at; i < end; ++i)
			{
				if (unknownApp(ids[i]))
				{
					response.add_unknown_appids(ids[i]);
					continue;
				}
				auto* app = response.add_apps();
				app->set_appid(ids[i]);
				app->set_change_number(ids[i] + 1000);
				app->set_buffer("app " + std::to_string(ids[i]));
				endpoint.delivered.insert(ids[i]);
			}
			const bool last = end == ids.size();
			response.set_response_pending(!last || endpoint.withholdFinal);
			if (endpoint.gzip)
			{
				const std::string inner = CmWire::packPacket(
				    8904, CMsgProtoBufHeader().SerializeAsString(), response.SerializeAsString());
				const auto size = static_cast<uint32_t>(inner.size());
				std::string body(reinterpret_cast<const char*>(&size), 4);
				body += inner;
				CMsgMulti multi;
				multi.set_size_unzipped(static_cast<uint32_t>(body.size()));
				multi.set_message_body(gzipped(body));
				queue(h, 1, CMsgProtoBufHeader().SerializeAsString(), multi.SerializeAsString());
			}
			else
			{
				// A length-delimited field that runs past the end.
				const bool corrupt = endpoint.corruptSecond && at == kAppsPerResponse;
				queue(h, 8904, CMsgProtoBufHeader().SerializeAsString(),
				      corrupt ? std::string("\x0a\x7f", 2) : response.SerializeAsString(),
				      endpoint.fragment);
			}
			if (last && endpoint.withholdFinal) h.dropped = true;
			if (last && endpoint.closeAfterFinal) h.rx += frame(0x88, "");
		}
	}
};

struct Fetch
{
	CmClient::FetchResult result = CmClient::FetchResult::Failed;
	std::unordered_map<uint32_t, std::string> buffers;
	std::unordered_map<uint32_t, uint32_t> changes;
};

Fetch fetch(ScriptedCms& cms, const std::vector<uint32_t>& ids)
{
	g_fakeCurl = &cms;
	Fetch f;
	f.result = CmClient::fetchProductInfoDetailed(ids, f.buffers, &f.changes);
	return f;
}

std::size_t knownIn(const std::vector<uint32_t>& ids)
{
	return static_cast<std::size_t>(std::count_if(
	    ids.begin(), ids.end(), [](uint32_t id) { return !unknownApp(id); }));
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char configHome[] = "/tmp/test_cmclient_session_XXXXXX";
	if (!mkdtemp(configHome)) return 2;
	setenv("XDG_CONFIG_HOME", configHome, 1);
	setenv("SLSSTEAM_CMLIST_TTL", "0", 1);
	const std::string logPath = std::string(configHome) + "/session.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	using CmClient::FetchResult;
	std::vector<uint32_t> batch(505);
	std::iota(batch.begin(), batch.end(), 1u);

	{
		ScriptedCms cms;
		cms.endpoints["cm-a.test"];
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, batch);
		CHECK(f.result == FetchResult::Success && f.buffers.size() == knownIn(batch) &&
		          f.changes.size() == f.buffers.size() && f.changes.at(1) == 1001,
		      "a healthy CM returns every app it has product info for");
		CHECK(cms.endpoints["cm-a.test"].requests.size() == 1 &&
		          cms.endpoints["cm-b.test"].requests.empty(),
		      "one healthy CM serves the batch in one request");
		CHECK(cms.directoryRedirect == CURLPROTO_HTTPS,
		      "the CM directory GET follows redirects to https only");
	}
	{
		ScriptedCms cms;
		cms.directoryPadding = 2u << 20;
		cms.endpoints["cm-a.test"];
		(void)fetch(cms, {1, 2, 3});
		CHECK(cms.directoryAccepted <= (1u << 20),
		      "a CM directory answer over 1 MiB is not buffered");
	}
	{
		// The first CM drops partway through its answer.
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].responsesLeft = 4;
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, batch);
		CHECK(f.result == FetchResult::Success && f.buffers.size() == knownIn(batch),
		      "the next CM completes the batch");
		const auto& sent = cms.endpoints["cm-a.test"].delivered;
		bool repeated = false;
		for (const auto& request : cms.endpoints["cm-b.test"].requests)
			for (uint32_t id : request) repeated = repeated || sent.count(id) != 0;
		CHECK(!sent.empty() && !repeated, "apps the first CM already sent are not asked again");
	}
	{
		// The first CM drops partway through, and no CM serves the rest.
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].responsesLeft = 3;
		cms.endpoints["cm-b.test"].refuse = true;
		const Fetch f = fetch(cms, batch);
		const auto& sent = cms.endpoints["cm-a.test"].delivered;
		bool kept = !sent.empty() && f.buffers.size() == sent.size();
		for (uint32_t id : sent) kept = kept && f.buffers.count(id) != 0;
		CHECK(f.result == FetchResult::Success && kept,
		      "a batch cut short keeps every app that arrived");
	}
	{
		// Every app arrives, then the socket drops before the final message.
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].withholdFinal = true;
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, {1, 2, 3});
		CHECK(f.result == FetchResult::Success && f.buffers.size() == 3 &&
		          cms.endpoints["cm-b.test"].requests.empty(),
		      "nothing is asked again once every app has arrived");
	}
	{
		// Every answer is compressed and there is no gzip program to run.
		const std::string noTools = std::string(configHome) + "/empty-path";
		std::filesystem::create_directories(noTools);
		const std::string path = std::getenv("PATH");
		setenv("PATH", noTools.c_str(), 1);
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].gzip = true;
		cms.endpoints["cm-b.test"].gzip = true;
		const Fetch f = fetch(cms, batch);
		setenv("PATH", path.c_str(), 1);
		CHECK(f.result == FetchResult::Success && f.buffers.size() == knownIn(batch),
		      "compressed answers are read in process");
	}
	{
		// A fragmented answer is not reassembled; the next CM serves it.
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].fragment = true;
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, batch);
		CHECK(f.result == FetchResult::Success && f.buffers.size() == knownIn(batch),
		      "a fragmented answer is fetched again from the next CM");
	}
	{
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].corruptSecond = true;
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, batch);
		CHECK(f.result == FetchResult::Success && f.buffers.size() == knownIn(batch),
		      "an unreadable answer is fetched again from the next CM");
	}
	{
		// The CM closes the socket right after its final answer.
		ScriptedCms cms;
		cms.endpoints["cm-a.test"].closeAfterFinal = true;
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, {1, 2, 10});
		CHECK(f.result == FetchResult::Success && f.buffers.size() == 2 &&
		          cms.endpoints["cm-b.test"].requests.empty(),
		      "a close after the final answer still completes the request");
	}
	{
		ScriptedCms cms;
		cms.endpoints["cm-a.test"];
		cms.endpoints["cm-b.test"];
		const Fetch f = fetch(cms, {10, 20, 30});
		CHECK(f.result == FetchResult::Success && f.buffers.empty(),
		      "an answer without product info is still an answer");
		CHECK(cms.endpoints["cm-a.test"].requests.size() == 1 &&
		          cms.endpoints["cm-b.test"].requests.empty(),
		      "a CM that answered every request is not asked again");
	}

	if (g_failures == 0) std::printf("\nall cmclient session checks passed\n");
	else std::printf("\n%d cmclient session check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
