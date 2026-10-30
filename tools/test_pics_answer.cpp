// The request hook hides the apps whose appinfo comes from a local pair from
// Steam's product-info requests. A metadata request is then answered for them
// from the pair (the change number and SHA-1 Steam already holds), so Steam
// stops asking again.
// Build: make test-pics-answer (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/apps.hpp"
#include "../src/feats/cmrecv.hpp"
#include "../src/log.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/steam.hpp"
#include "../src/sdk/protobufs/steammessages_clientserver_appinfo.pb.h"

#include "base64/base64.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

namespace
{

constexpr uint32_t kApp = 42;
constexpr uint32_t kOther = 50;

// A packet as Steam's send and receive hooks see it.
template <typename Message>
void fill(CNetPacket& packet, uint32_t eMsg, const CMsgProtoBufHeader& header,
          const Message& message)
{
	const auto headerSize = static_cast<uint32_t>(header.ByteSizeLong());
	const auto messageSize = static_cast<uint32_t>(message.ByteSizeLong());
	const auto size = static_cast<uint32_t>(sizeof(CNetPacketBody) + headerSize + messageSize);
	auto* bytes = static_cast<uint8_t*>(Steam::Plat_Alloc(static_cast<int>(size)));
	auto* body = reinterpret_cast<CNetPacketBody*>(bytes);
	body->type = static_cast<ENetPacket>(eMsg | CNetPacket::PROTOBUF_TYPE_MASK);
	body->headerSize = headerSize;
	header.SerializeToArray(bytes + sizeof(CNetPacketBody), static_cast<int>(headerSize));
	message.SerializeToArray(bytes + sizeof(CNetPacketBody) + headerSize,
	                         static_cast<int>(messageSize));
	packet.setBody(body);
	packet.setOriginalBody(body);
	packet.setSize(size);
}

// Steam asks for the metadata of 42 (local pair) and 50 in job `job`; the
// CM answers job `answeredJob` for 50 only. Returns what Steam receives.
CMsgClientPICSProductInfoResponse exchange(uint64_t job, uint64_t answeredJob,
                                           bool metadataOnly,
                                           CMsgClientPICSProductInfoRequest* sent)
{
	CMsgProtoBufHeader requestHeader;
	requestHeader.set_jobid_source(job);
	CMsgClientPICSProductInfoRequest request;
	request.add_apps()->set_appid(kApp);
	request.add_apps()->set_appid(kOther);
	request.set_meta_data_only(metadataOnly);
	CNetPacket out{};
	fill(out, k_EMsgClientPICSProductInfoRequest, requestHeader, request);
	Apps::sendMsg(&out);
	if (sent) *sent = out.deserializeBody<CMsgClientPICSProductInfoRequest>();
	out.free();

	CMsgProtoBufHeader responseHeader;
	responseHeader.set_jobid_target(answeredJob);
	CMsgClientPICSProductInfoResponse response;
	response.set_meta_data_only(metadataOnly);
	auto* other = response.add_apps();
	other->set_appid(kOther);
	other->set_change_number(3);
	other->set_sha(std::string(20, 'b'));
	CNetPacket in{};
	fill(in, k_EMsgClientPICSProductInfoResponse, responseHeader, response);
	CmRecv::dispatchNetPacket(&in);
	auto received = in.deserializeBody<CMsgClientPICSProductInfoResponse>();
	in.free();
	return received;
}

const CMsgClientPICSProductInfoResponse_AppInfo* entryFor(
	const CMsgClientPICSProductInfoResponse& response, uint32_t appId)
{
	for (const auto& app : response.apps())
		if (app.appid() == appId) return &app;
	return nullptr;
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_pics_answer_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	Steam::Plat_Alloc = [](int size) -> void* { return std::malloc(static_cast<std::size_t>(size)); };
	Steam::Plat_Free = [](void* memory) { std::free(memory); };

	// App 42's validated pair.
	const std::string cache = std::string(home) + "/SLSsteam/cache";
	std::filesystem::create_directories(cache);
	const std::string wire =
		"\"appinfo\"\n{\n\t\"appid\"\t\t\"42\"\n\t\"depots\"\n\t{\n\t\t\"43\"\n\t\t{\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\t\"1234567890\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
	std::uint8_t sha[20]{};
	AppInfoProvision::sha1Bytes(wire.data(), wire.size(), sha);
	const std::string shaBytes(reinterpret_cast<const char*>(sha), sizeof sha);
	std::ofstream(cache + "/picsbuffer_42.bin", std::ios::binary) << wire;
	std::ofstream(cache + "/picsbuffer_42.yaml")
		<< "appid: 42\nchange_number: 7\nwire_size: " << wire.size()
		<< "\nsha_b64: " << base64::to_base64(shaBytes)
		<< "\nnormalized: true\nsynthetic: false\n";
	g_config.addedAppIds.set({kApp});
	g_config.managedAppIds.set({kApp});
	CHECK(AppInfoProvision::probeCache(kApp, AppInfoProvision::CacheProbeMode::BlockingValidate)
	          .readiness != AppInfoProvision::CacheReadiness::Invalid,
	      "app 42's pair is valid");

	CMsgClientPICSProductInfoRequest sent;
	const auto answered = exchange(77, 77, true, &sent);
	CHECK(sent.apps_size() == 1 && sent.apps(0).appid() == kOther,
	      "the metadata request goes out without the app that has a local pair");
	const auto* entry = entryFor(answered, kApp);
	CHECK(entry && entry->change_number() == 7 && entry->sha() == shaBytes &&
	      entry->buffer().empty() && entryFor(answered, kOther),
	      "Steam receives app 42's metadata from its pair with the CM's answer");

	CHECK(!entryFor(exchange(78, 79, true, nullptr), kApp),
	      "an answer to another job gets nothing added");
	CHECK(!entryFor(exchange(80, 80, false, nullptr), kApp),
	      "a request for full data is not answered from the pair");

	if (g_failures == 0) std::printf("\nall pics answer checks passed\n");
	else std::printf("\n%d pics answer check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
