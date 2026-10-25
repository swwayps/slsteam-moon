// Exercise real wire handlers and the license policy; stub only Steam/config.
#include "../src/config.hpp"
#include "../src/feats/achievements.hpp"
#include "../src/feats/stats_policy.hpp"
#include "../src/feats/playerstats.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/CProtoBufMsgBase.hpp"
#include "../src/sdk/CUser.hpp"
#include "../src/sdk/steam.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
constexpr uint32_t app = 12345;
constexpr uint64_t self = 76561198000000001ULL, other = self + 1, reviewer = self + 98;
std::vector<std::pair<uint32_t, uint32_t>> lookups;
int saves = 0;
int failures = 0;
bool injected = true, available = true, success = false, excluded = false;
AppOwnershipInfo_t info{};
CUser user;
uint64_t nextJob = 100;
void check(bool ok, const char* label) {
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++failures;
}
void license(int32_t package, bool owns = true, bool expired = false) {
	StatsPolicy::invalidate();
	info = {}; info.subId = package; info.ownsLicense = owns; info.licenseExpired = expired;
	success = package >= 0;
}
std::vector<uint8_t> request(bool modern, uint64_t job, uint64_t target = self, uint32_t game = app) {
	CMsgProtoBufHeader header;
	header.set_steamid(self); header.set_jobid_source(job);
	std::vector<uint8_t> bytes;
	if (modern) bytes = PlayerStats::buildSpoofedRequest(target, game);
	else {
		CMsgClientGetUserStats body;
		body.set_game_id(game); body.set_steam_id_for_user(target);
		body.set_crc_stats(123); body.set_schema_local_version(7);
		auto raw = body.SerializeAsString(); bytes.assign(raw.begin(), raw.end());
	}
	const auto before = bytes;
	auto out = Achievements::rewriteRequest(modern, bytes.data(), bytes.size(), header);
	check(before == bytes, "input buffer remains untouched");
	return out;
}
std::optional<std::vector<uint8_t>> response(bool modern, uint64_t job, bool schema = true,
                                             int32_t result = 1, uint32_t game = app) {
	CMsgProtoBufHeader header;
	header.set_steamid(self); header.set_jobid_target(job); header.set_eresult(result);
	std::vector<uint8_t> bytes;
	if (modern) {
		bytes = {0x10, 123, 0x22, 2, 8, 1}; // crc + stats
		if (schema) bytes.insert(bytes.end(), {0x1a, 1, 0x42});
	} else {
		CMsgClientGetUserStatsResponse body;
		body.set_game_id(game); body.set_eresult(result); body.set_crc_stats(123);
		body.add_stats()->set_stat_id(1); body.add_achievement_blocks()->set_achievement_id(1);
		if (schema) body.set_schema("schema");
		auto raw = body.SerializeAsString(); bytes.assign(raw.begin(), raw.end());
	}
	return Achievements::rewriteResponse(modern, bytes.data(), bytes.size(), header);
}
uint64_t targetOf(bool modern, const std::vector<uint8_t>& body) {
	if (body.empty()) return 0;
	if (modern) return PlayerStats::parseRequestSteamId(body.data(), body.size()).value_or(0);
	CMsgClientGetUserStats parsed;
	return parsed.ParseFromArray(body.data(), body.size()) ? parsed.steam_id_for_user() : 0;
}
// A Steam-shaped CM packet: CNetPacketBody{type, headerSize} + header + body.
CNetPacket packet(uint32_t eMsg, const CMsgProtoBufHeader& header, const std::vector<uint8_t>& body) {
	const std::string head = header.SerializeAsString();
	const auto size = static_cast<uint32_t>(sizeof(CNetPacketBody) + head.size() + body.size());
	auto* memory = static_cast<uint8_t*>(Steam::Plat_Alloc(static_cast<int>(size)));
	auto* frame = reinterpret_cast<CNetPacketBody*>(memory);
	frame->type = static_cast<ENetPacket>(eMsg | PROTOBUF_TYPE_MASK);
	frame->headerSize = static_cast<uint32_t>(head.size());
	std::memcpy(memory + sizeof(CNetPacketBody), head.data(), head.size());
	if (!body.empty()) std::memcpy(memory + sizeof(CNetPacketBody) + head.size(), body.data(), body.size());
	CNetPacket out{};
	out.setBody(frame);
	out.setSize(size);
	return out;
}
std::vector<uint8_t> packetBody(const CNetPacket& pkt) {
	const auto* frame = reinterpret_cast<const uint8_t*>(pkt.body());
	const uint32_t offset = sizeof(CNetPacketBody) + pkt.body()->headerSize;
	return {frame + offset, frame + pkt.sizeBytes()};
}
bool safeFailure(bool modern, const std::optional<std::vector<uint8_t>>& output) {
	if (!output) return false;
	if (modern) return output->empty();
	CMsgClientGetUserStatsResponse body;
	return body.ParseFromArray(output->data(), output->size()) && body.eresult() == 2 &&
	       !body.has_schema() && !body.stats_size() && !body.achievement_blocks_size();
}
}
CConfig g_config;
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CConfig::~CConfig() = default;
CLog::CLog(const char* path) { if (path && *path) ofstream.open(path); }
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
bool CConfig::isAddedAppId(uint32_t id) { return addedAppIds.contains(id); }
bool CConfig::shouldExcludeAppId(uint32_t, bool) { return excluded; }
CUser* getLocalUser() { return available ? &user : nullptr; }
bool CUser::checkAppOwnership(uint32_t, AppOwnershipInfo_t* out) { *out = info; return success; }
namespace PackagePatch { bool isInjectedAppId(uint32_t) { return injected; } }
void Achievements::requestOwnerLookup(uint32_t game, uint32_t count) { lookups.emplace_back(game, count); }
void Achievements::requestOwnerSave() { ++saves; }
namespace Steam {
Plat_Alloc_t Plat_Alloc = [](int size) -> void* { return std::malloc(static_cast<std::size_t>(size)); };
Plat_Free_t Plat_Free = [](void* memory) { std::free(memory); };
Plat_Realloc_t Plat_Realloc = nullptr;
}

int main() {
	g_config.achievements.set(true); g_config.addedAppIds.set({app});
	g_config.achievementOwnerId.set(other);
	StatsPolicy::setAccount(static_cast<uint32_t>(self));
	for (bool modern : {false, true}) {
		license(-1, false);
		check(request(modern, nextJob++).empty(), "unknown license passes through");
		license(42);
		check(request(modern, nextJob++).empty(), "owned game added for DLC passes through");
		license(42, false, true);
		check(request(modern, nextJob++).empty(), "expired real license stays official");
		license(0); injected = false;
		check(request(modern, nextJob++).empty(), "native package-zero app stays official");
		injected = true; available = false;
		check(request(modern, nextJob++).empty(), "missing native user fails closed");
		available = true;
		check(request(modern, nextJob++, other).empty(), "another user's query passes through");
		check(request(modern, UINT64_MAX).empty(), "missing job ID passes through");
		excluded = true;
		check(request(modern, nextJob++).empty(), "excluded app passes through"); excluded = false;
		g_config.addedAppIds.set({});
		check(request(modern, nextJob++).empty(), "unmanaged app passes through");
		g_config.addedAppIds.set({app});
		uint64_t first = nextJob++, second = nextJob++;
		check(!request(modern, first).empty() && !request(modern, second).empty(),
		      "confirmed local app retains parallel schema requests");
		check(!response(modern, nextJob++), "unrelated same-app response stays intact");
		auto reply = response(modern, second);
		if (modern) check(reply && !PlayerStats::hasField(reply->data(), reply->size(), 4) &&
		    !PlayerStats::fieldBytes(reply->data(), reply->size(), 3).empty(), "modern reply keeps schema, not owner progress");
		else {
			CMsgClientGetUserStatsResponse parsed;
			check(reply && parsed.ParseFromArray(reply->data(), reply->size()) && parsed.schema() == "schema" &&
			      parsed.stats_size() == 0 && parsed.achievement_blocks_size() == 0,
			      "legacy reply keeps schema, not owner progress");
		}
		check(response(modern, first).has_value(), "out-of-order first reply is correlated independently");
		check(!response(modern, first), "a reply is consumed only once");
		first = nextJob++; request(modern, first); license(77);
		check(safeFailure(modern, response(modern, first)), "purchase in flight rejects stale owner progress");
		license(0); first = nextJob++; request(modern, first);
		StatsPolicy::setAccount(static_cast<uint32_t>(other));
		check(safeFailure(modern, response(modern, first)), "account switch rejects stale owner progress");
		StatsPolicy::setAccount(static_cast<uint32_t>(self));
		first = nextJob++; request(modern, first);
		check(safeFailure(modern, response(modern, first, false)), "empty schema is never reported as successful zero unlocks");
	}
	CMsgProtoBufHeader header; header.set_jobid_source(nextJob++);
	auto bad = PlayerStats::buildSpoofedRequest(self, app); bad.push_back(0x80);
	check(Achievements::rewriteRequest(true, bad.data(), bad.size(), header).empty(), "truncated modern request passes through");
	for (bool modern : {false, true}) {
		license(0);
		const auto job = nextJob++;
		request(modern, job);
		CMsgProtoBufHeader replyHeader;
		replyHeader.set_jobid_target(job);
		std::vector<uint8_t> large(Achievements::maxReplyBytes + 1, 0);
		check(safeFailure(modern, Achievements::rewriteResponse(modern, large.data(), large.size(), replyHeader)) &&
		      replyHeader.eresult() == 2, "oversized owner reply fails closed before the frame buffer limit");
		const auto dup = nextJob++;
		request(modern, dup); request(modern, dup);
		check(safeFailure(modern, response(modern, dup)), "ambiguous duplicate job never publishes another user's progress");
	}
	for (bool modern : {false, true}) {
		const uint32_t game = modern ? 54322 : 54321;
		g_config.addedAppIds.set({app, game}); g_config.maxSchemaTries.set(5);
		license(0);
		auto job = nextJob++;
		check(targetOf(modern, request(modern, job, self, game)) == other,
		      "the configured fallback owner is asked first");
		saves = 0;
		check(safeFailure(modern, response(modern, job, false, 2, game)) && saves == 1,
		      "a refusing owner fails closed and is remembered");
		lookups.clear();
		check(request(modern, nextJob++, self, game).empty() && lookups.size() == 1 &&
		      lookups[0] == std::make_pair(game, 5u),
		      "a refused fallback passes through and starts one background lookup");
		check(request(modern, nextJob++, self, game).empty() && lookups.size() == 1,
		      "the lookup is not repeated while it runs");
		Achievements::offerOwners(game, {reviewer});
		check(saves == 2, "looked-up reviewers are saved");
		job = nextJob++;
		check(targetOf(modern, request(modern, job, self, game)) == reviewer,
		      "the next request asks the first reviewer");
		check(response(modern, job, true, 1, game).has_value() && saves == 3,
		      "the reviewer's schema is delivered and the reviewer is remembered");
		check(targetOf(modern, request(modern, nextJob++, self, game)) == reviewer,
		      "the reviewer that answered is reused");
		g_config.addedAppIds.set({app});
	}
	{
		// Owner replies arrive as CM packets; the rewrite must land in them.
		license(0);
		const auto job = nextJob++;
		check(!request(true, job).empty(), "a modern request is rewritten for the packet check");
		CMsgProtoBufHeader header;
		header.set_steamid(self); header.set_jobid_target(job); header.set_eresult(1);
		auto reply = packet(147, header, {0x10, 123, 0x22, 2, 8, 1, 0x1a, 1, 0x42});
		check(Achievements::recvReply(&reply), "a modern owner reply packet is rewritten");
		const auto body = packetBody(reply);
		check(!PlayerStats::hasField(body.data(), body.size(), 4) &&
		      !PlayerStats::hasField(body.data(), body.size(), 2) &&
		      !PlayerStats::fieldBytes(body.data(), body.size(), 3).empty(),
		      "the packet keeps the schema and loses the owner's progress");
		const auto legacyJob = nextJob++;
		check(!request(false, legacyJob).empty(), "a legacy request is rewritten for the packet check");
		CMsgProtoBufHeader legacyHeader;
		legacyHeader.set_steamid(self); legacyHeader.set_jobid_target(legacyJob); legacyHeader.set_eresult(1);
		CMsgClientGetUserStatsResponse refused;
		refused.set_game_id(app); refused.set_eresult(2);
		const auto refusedBytes = refused.SerializeAsString();
		auto legacy = packet(819, legacyHeader,
			std::vector<uint8_t>(refusedBytes.begin(), refusedBytes.end()));
		check(Achievements::recvReply(&legacy), "a legacy owner reply packet is rewritten");
		CMsgProtoBufHeader legacyOut;
		CMsgClientGetUserStatsResponse legacyBody;
		const auto legacyBytes = packetBody(legacy);
		check(legacy.deserializeHeader(legacyOut) && legacyOut.eresult() == 2 &&
		      legacyBody.ParseFromArray(legacyBytes.data(), static_cast<int>(legacyBytes.size())) &&
		      legacyBody.eresult() == 2 && !legacyBody.has_schema(),
		      "a refused legacy reply fails closed in the packet");
		CMsgProtoBufHeader unrelated;
		unrelated.set_jobid_target(nextJob++); unrelated.set_eresult(1);
		auto other = packet(147, unrelated, {0x1a, 1, 0x42});
		const auto* before = other.body();
		check(!Achievements::recvReply(&other) && other.body() == before,
		      "an unrelated service reply packet is left untouched");
	}
	// Unanswered requests are kept up to a cap; reaching it is reported once
	// instead of silently ending the rewrite for the session.
	{
		const std::string capLog = "/tmp/test_achievement_scope_cap." + std::to_string(getpid());
		g_pLog = std::make_unique<CLog>(capLog.c_str());
		// A game no earlier case touched, so its configured owner is asked.
		constexpr uint32_t capGame = 54400;
		license(0); injected = true; available = true; excluded = false;
		g_config.addedAppIds.set({capGame});
		const auto rewrite = [](uint64_t job) {
			CMsgProtoBufHeader header;
			header.set_steamid(self); header.set_jobid_source(job);
			const auto bytes = PlayerStats::buildSpoofedRequest(self, capGame);
			return Achievements::rewriteRequest(true, bytes.data(), bytes.size(), header);
		};
		bool capped = false;
		for (int i = 0; i < 5000 && !capped; ++i) capped = rewrite(nextJob++).empty();
		rewrite(nextJob++);
		g_pLog.reset();
		std::ifstream in(capLog);
		int reports = 0;
		for (std::string line; std::getline(in, line);)
			if (line.find("unanswered") != std::string::npos) ++reports;
		std::remove(capLog.c_str());
		check(capped && reports == 1, "a full request table is reported once");
	}
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
