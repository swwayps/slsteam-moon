// Exercise real wire handlers and the license policy; stub only Steam/config.
#include "../src/config.hpp"
#include "../src/feats/achievements.hpp"
#include "../src/feats/stats_policy.hpp"
#include "../src/feats/playerstats.hpp"
#include "../src/sdk/CProtoBufMsgBase.hpp"
#include "../src/sdk/CAppOwnershipInfo.hpp"
#include "../src/sdk/CUser.hpp"
#include <cstdio>
#include <memory>

namespace {
constexpr uint32_t app = 12345;
constexpr uint64_t self = 76561198000000001ULL, other = self + 1;
int failures = 0;
bool injected = true, available = true, success = false, excluded = false;
CAppOwnershipInfo info{};
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
std::vector<uint8_t> request(bool modern, uint64_t job, uint64_t target = self) {
	CMsgProtoBufHeader header;
	header.set_steamid(self); header.set_jobid_source(job);
	std::vector<uint8_t> bytes;
	if (modern) bytes = PlayerStats::buildSpoofedRequest(target, app);
	else {
		CMsgClientGetUserStats body;
		body.set_game_id(app); body.set_steam_id_for_user(target);
		body.set_crc_stats(123); body.set_schema_local_version(7);
		auto raw = body.SerializeAsString(); bytes.assign(raw.begin(), raw.end());
	}
	const auto before = bytes;
	auto out = Achievements::rewriteRequest(modern, bytes.data(), bytes.size(), header);
	check(before == bytes, "input buffer remains untouched");
	return out;
}
std::optional<std::vector<uint8_t>> response(bool modern, uint64_t job, bool schema = true) {
	CMsgProtoBufHeader header;
	header.set_steamid(self); header.set_jobid_target(job); header.set_eresult(1);
	std::vector<uint8_t> bytes;
	if (modern) {
		bytes = {0x10, 123, 0x22, 2, 8, 1}; // crc + stats
		if (schema) bytes.insert(bytes.end(), {0x1a, 1, 0x42});
	} else {
		CMsgClientGetUserStatsResponse body;
		body.set_game_id(app); body.set_eresult(1); body.set_crc_stats(123);
		body.add_stats()->set_stat_id(1); body.add_achievement_blocks()->set_achievement_id(1);
		if (schema) body.set_schema("schema");
		auto raw = body.SerializeAsString(); bytes.assign(raw.begin(), raw.end());
	}
	return Achievements::rewriteResponse(modern, bytes.data(), bytes.size(), header);
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
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
bool CConfig::isAddedAppId(uint32_t id) { return addedAppIds.contains(id); }
bool CConfig::shouldExcludeAppId(uint32_t) { return excluded; }
CUser* getLocalUser() { return available ? &user : nullptr; }
bool CUser::checkAppOwnership(uint32_t, CAppOwnershipInfo* out) { *out = info; return success; }
namespace PackagePatch { bool isInjectedAppId(uint32_t) { return injected; } }

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
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
