// The request hook hides the apps whose appinfo comes from a local pair from
// Steam's product-info requests. A metadata request is then answered for them
// from the pair (the change number and SHA-1 Steam already holds), so Steam
// stops asking again.
// Build: make test-pics-answer (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/appinfo_vdf.hpp"
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
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

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
	// The receive hook settles the field layout on the first live packet.
	CNetPacket::detectLayout(&packet);
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

	// App 42's pair, spliced into appinfo.vdf as at boot: Steam holds change
	// 7 and this SHA-1.
	const std::string cache = std::string(home) + "/SLSsteam/cache";
	std::filesystem::create_directories(cache);
	const auto pairForApp = [&](uint32_t appId, const char* gid, uint32_t change)
	{
		const std::string idStr = std::to_string(appId);
		const std::string depotStr = std::to_string(appId + 1);
		const std::string wire =
			"\"appinfo\"\n{\n\t\"appid\"\t\t\"" + idStr + "\"\n\t\"depots\"\n\t{\n\t\t\"" +
			depotStr + "\"\n\t\t{\n"
			"\t\t\t\"manifests\"\n\t\t\t{\n\t\t\t\t\"public\"\n\t\t\t\t{\n"
			"\t\t\t\t\t\"gid\"\t\t\"" + std::string(gid) + "\"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
		std::uint8_t sha[20]{};
		AppInfoProvision::sha1Bytes(wire.data(), wire.size(), sha);
		const std::string shaBytes(reinterpret_cast<const char*>(sha), sizeof sha);
		std::ofstream(cache + "/picsbuffer_" + idStr + ".bin.tmp", std::ios::binary) << wire;
		std::filesystem::rename(cache + "/picsbuffer_" + idStr + ".bin.tmp",
		                        cache + "/picsbuffer_" + idStr + ".bin");
		std::ofstream(cache + "/picsbuffer_" + idStr + ".yaml.tmp")
			<< "appid: " << appId << "\nchange_number: " << change << "\nwire_size: " << wire.size()
			<< "\nsha_b64: " << base64::to_base64(shaBytes)
			<< "\nnormalized: true\nsynthetic: false\n";
		std::filesystem::rename(cache + "/picsbuffer_" + idStr + ".yaml.tmp",
		                        cache + "/picsbuffer_" + idStr + ".yaml");
		return shaBytes;
	};
	const auto pairFor = [&](const char* gid, uint32_t change)
	{
		return pairForApp(kApp, gid, change);
	};
	const std::string shaBytes = pairFor("1234567890", 7);
	g_config.addedAppIds.set({kApp});
	g_config.managedAppIds.set({kApp});
	{
		const std::string appinfo = std::string(home) + "/appinfo.vdf";
		std::ofstream out(appinfo, std::ios::binary);
		const uint32_t magic = 0x07564429u, universe = 1, footer = 0, strings = 0;
		const int64_t tableOffset = 20;
		out.write(reinterpret_cast<const char*>(&magic), 4);
		out.write(reinterpret_cast<const char*>(&universe), 4);
		out.write(reinterpret_cast<const char*>(&tableOffset), 8);
		out.write(reinterpret_cast<const char*>(&footer), 4);
		out.write(reinterpret_cast<const char*>(&strings), 4);
		out.close();
		CHECK(AppInfoVdf::injectAllCached(appinfo) == 1, "the boot splice gives Steam app 42");
	}
	// A later refresh rewrites the pair; it is not published live, so Steam
	// still holds change 7.
	pairFor("1234567891", 8);
	CHECK(AppInfoProvision::probeCache(kApp, AppInfoProvision::CacheProbeMode::BlockingValidate)
	          .readiness != AppInfoProvision::CacheReadiness::Invalid,
	      "app 42's rewritten pair is valid");

	CMsgClientPICSProductInfoRequest sent;
	const auto answered = exchange(77, 77, true, &sent);
	CHECK(sent.apps_size() == 1 && sent.apps(0).appid() == kOther,
	      "the metadata request goes out without the app that has a local pair");
	const auto* entry = entryFor(answered, kApp);
	CHECK(entry && entry->change_number() == 7 && entry->sha() == shaBytes &&
	      entry->buffer().empty() && entryFor(answered, kOther),
	      "Steam receives the metadata it holds for app 42 with the CM's answer");

	// A runtime splice of the new pair counts once Steam reloaded the file.
	{
		const std::string appinfo = std::string(home) + "/appinfo.vdf";
		const bool spliced = AppInfoVdf::injectCachedApps(appinfo, {kApp}) == 1;
		const auto first = exchange(81, 81, true, nullptr);
		const auto* before = entryFor(first, kApp);
		const bool unchanged = before && before->change_number() == 7;
		AppInfoVdf::commitLiveEntries({kApp});
		const auto second = exchange(82, 82, true, nullptr);
		const auto* after = entryFor(second, kApp);
		CHECK(spliced && unchanged && after && after->change_number() == 8,
		      "a runtime splice changes the answer only after Steam reloaded it");
	}

	CHECK(!entryFor(exchange(78, 79, true, nullptr), kApp),
	      "an answer to another job gets nothing added");
	CHECK(!entryFor(exchange(80, 80, false, nullptr), kApp),
	      "a request for full data is not answered from the pair");

	// R3-P-1(a): When another login instance holds the appinfo.vdf lock briefly,
	// the surviving process waits for it and records the spliced entries instead
	// of leaving hidden apps unanswered.
	{
		const std::string lockPath = std::string(home) + "/appinfo.vdf.slssteam.lock";
		int readyPipe[2]{};
		if (pipe(readyPipe) != 0) return 2;
		std::fflush(stdout);
		const pid_t holder = fork();
		if (holder == 0)
		{
			close(readyPipe[0]);
			const int fd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
			if (fd < 0 || flock(fd, LOCK_EX) != 0) std::_Exit(2);
			(void)!write(readyPipe[1], "1", 1);
			close(readyPipe[1]);
			usleep(200 * 1000);
			std::_Exit(0);
		}
		close(readyPipe[1]);
		char ready = 0;
		(void)!read(readyPipe[0], &ready, 1);
		close(readyPipe[0]);

		const pid_t survivor = fork();
		if (survivor == 0)
		{
			const std::string sha43Bytes = pairForApp(43, "2234567890", 11);
			g_config.addedAppIds.set({kApp, 43});
			g_config.managedAppIds.set({kApp, 43});
			const int n = AppInfoVdf::injectAllCached(std::string(home) + "/appinfo.vdf");

			CMsgProtoBufHeader reqHdr;
			reqHdr.set_jobid_source(91);
			CMsgClientPICSProductInfoRequest req;
			req.add_apps()->set_appid(43);
			req.add_apps()->set_appid(kOther);
			req.set_meta_data_only(true);
			CNetPacket outPkt{};
			fill(outPkt, k_EMsgClientPICSProductInfoRequest, reqHdr, req);
			Apps::sendMsg(&outPkt);
			const auto sentReq = outPkt.deserializeBody<CMsgClientPICSProductInfoRequest>();
			outPkt.free();

			CMsgProtoBufHeader respHdr;
			respHdr.set_jobid_target(91);
			CMsgClientPICSProductInfoResponse resp;
			resp.set_meta_data_only(true);
			resp.add_apps()->set_appid(kOther);
			CNetPacket inPkt{};
			fill(inPkt, k_EMsgClientPICSProductInfoResponse, respHdr, resp);
			CmRecv::dispatchNetPacket(&inPkt);
			const auto got = inPkt.deserializeBody<CMsgClientPICSProductInfoResponse>();
			inPkt.free();
			const auto* e43 = entryFor(got, 43);
			const bool ok = n == 2 && sentReq.apps_size() == 1 &&
			                sentReq.apps(0).appid() == kOther &&
			                e43 && e43->change_number() == 11 && e43->sha() == sha43Bytes;
			std::_Exit(ok ? 0 : 1);
		}
		int holderStatus = 0, survivorStatus = 0;
		waitpid(holder, &holderStatus, 0);
		waitpid(survivor, &survivorStatus, 0);
		std::filesystem::remove(cache + "/picsbuffer_43.bin");
		std::filesystem::remove(cache + "/picsbuffer_43.yaml");
		CHECK(WIFEXITED(survivorStatus) && WEXITSTATUS(survivorStatus) == 0,
		      "the boot splice waits for a competing writer and answers the spliced app");
	}

	// R3-P-1(b): A pair written after the boot splice (such as a startup repair)
	// that this process never spliced into appinfo.vdf is not hidden unanswered.
	{
		constexpr uint32_t kUnspliced = 44;
		pairForApp(kUnspliced, "3234567890", 9);
		g_config.addedAppIds.set({kApp, kUnspliced});
		g_config.managedAppIds.set({kApp, kUnspliced});

		CMsgProtoBufHeader reqHdr;
		reqHdr.set_jobid_source(92);
		CMsgClientPICSProductInfoRequest req;
		req.add_apps()->set_appid(kUnspliced);
		req.add_apps()->set_appid(kOther);
		req.set_meta_data_only(true);
		CNetPacket outPkt{};
		fill(outPkt, k_EMsgClientPICSProductInfoRequest, reqHdr, req);
		Apps::sendMsg(&outPkt);
		const auto sentReq = outPkt.deserializeBody<CMsgClientPICSProductInfoRequest>();
		outPkt.free();
		bool keptUnspliced = false;
		for (const auto& a : sentReq.apps())
			if (a.appid() == kUnspliced) keptUnspliced = true;
		CHECK(keptUnspliced,
		      "an app whose pair was never spliced in this process stays in Steam's request");

		// A staged runtime splice answers until committed, and un-hides when dropped.
		const std::string appinfo = std::string(home) + "/appinfo.vdf";
		const bool staged44 = AppInfoVdf::injectCachedApps(appinfo, {kUnspliced}) == 1;
		AppInfoVdf::dropStagedEntries({kUnspliced});
		CNetPacket afterDrop{};
		fill(afterDrop, k_EMsgClientPICSProductInfoRequest, reqHdr, req);
		Apps::sendMsg(&afterDrop);
		const auto afterDropReq = afterDrop.deserializeBody<CMsgClientPICSProductInfoRequest>();
		afterDrop.free();
		bool unhiddenAfterDrop = false;
		for (const auto& a : afterDropReq.apps())
			if (a.appid() == kUnspliced) unhiddenAfterDrop = true;
		CHECK(staged44 && unhiddenAfterDrop,
		      "dropping an uncommitted runtime splice leaves the app unhidden again");
	}

	if (g_failures == 0) std::printf("\nall pics answer checks passed\n");
	else std::printf("\n%d pics answer check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
