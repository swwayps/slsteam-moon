#include "achievements.hpp"
#include "stats_policy.hpp"
#include "playerstats.hpp"
#include "../config.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/EResult.hpp"

#include <mutex>

namespace
{
struct Pending { uint32_t app, account; uint64_t epoch; bool modern; };
std::mutex pendingMutex;
std::unordered_map<uint64_t, Pending> pending;
bool validJob(uint64_t job) { return job != 0 && job != UINT64_MAX; }
}

// At the serialized CM boundary the jobid is final, and CloudRedirect has
// already had the opportunity to serve the original SELF request. Work on
// our own protobuf objects, never Steam-owned strings/arenas.
std::vector<uint8_t> Achievements::rewriteRequest(bool modern,
	const uint8_t* data, uint32_t size, const CMsgProtoBufHeader& header)
{
	if (!g_config.achievements.get() || size > maxReplyBytes ||
	    header.ByteSizeLong() > maxHeaderBytes || !header.has_jobid_source() ||
	    !validJob(header.jobid_source())) return {};
	const uint32_t account = StatsPolicy::account();
	if (!account || (header.has_steamid() &&
	    !StatsPolicy::isSelf(header.steamid(), account))) return {};
	uint32_t app = 0;
	uint64_t target = 0;
	CMsgClientGetUserStats legacy;
	if (modern)
	{
		bool appSeen = false, targetSeen = false, valid = true;
		if (!PlayerStats::detail::walk(data, size, [&](const auto& field) {
			if (field.number != 1 && field.number != 2) return;
			bool& seen = field.number == 1 ? targetSeen : appSeen;
			if (seen || field.wireType != 0) { valid = false; return; }
			seen = true;
			std::size_t offset = field.valueOff;
			uint64_t value = 0;
			if (!PlayerStats::detail::readVarint(data, size, offset, value) ||
			    (field.number == 2 && (!value || value > UINT32_MAX))) valid = false;
		}) || !valid) return {};
		auto id = PlayerStats::parseRequestAppId(data, size);
		if (!id) return {};
		app = *id;
		target = PlayerStats::parseRequestSteamId(data, size).value_or(0);
	}
	else
	{
		if (!legacy.ParseFromArray(data, size) || !legacy.has_game_id() ||
		    legacy.game_id() > UINT32_MAX) return {};
		app = static_cast<uint32_t>(legacy.game_id());
		target = legacy.steam_id_for_user();
	}
	if (!StatsPolicy::isSelf(target, account)) return {};
	const uint64_t epoch = StatsPolicy::localEpoch(app, account, true);
	if (!epoch) return {};
	const uint64_t owner = resolveOwnerSteamId(app, g_config.achievementOwners.get(),
	                                          g_config.achievementOwnerId.get());
	if (!owner || StatsPolicy::isSelf(owner, account)) return {};
	std::vector<uint8_t> out;
	if (modern) out = PlayerStats::buildSpoofedRequest(owner, app);
	else
	{
		legacy.clear_crc_stats();
		legacy.set_schema_local_version(-1);
		legacy.set_steam_id_for_user(owner);
		out.resize(legacy.ByteSizeLong());
		if (!legacy.SerializeToArray(out.data(), out.size())) return {};
	}
	std::lock_guard lock(pendingMutex);
	// Retain unanswered jobs as bounded tombstones. Expiring by time would
	// allow a late owner reply through with its achievement progress intact.
	if (auto it = pending.find(header.jobid_source()); it != pending.end())
	{
		it->second.epoch = 0; // Ambiguous duplicate: any reply must fail safely.
		return {};
	}
	if (pending.size() >= 4096 || out.size() > maxReplyBytes) return {};
	pending.emplace(header.jobid_source(), Pending{app, account, epoch, modern});
	return out;
}

std::optional<std::vector<uint8_t>> Achievements::rewriteResponse(bool modern,
	const uint8_t* data, uint32_t size, CMsgProtoBufHeader& header)
{
	if (!header.has_jobid_target() || !validJob(header.jobid_target())) return {};
	Pending request{};
	{
		std::lock_guard lock(pendingMutex);
		auto it = pending.find(header.jobid_target());
		if (it == pending.end() || it->second.modern != modern) return {};
		request = it->second;
		pending.erase(it);
	}
	const auto failure = [&]() -> std::vector<uint8_t> {
		if (header.ByteSizeLong() > maxHeaderBytes)
		{
			const auto job = header.jobid_target();
			const auto steam = header.steamid();
			const auto session = header.client_sessionid();
			header.Clear();
			header.set_jobid_target(job);
			header.set_steamid(steam);
			header.set_client_sessionid(session);
		}
		header.set_eresult(2); // A rewritten request must NEVER leak another user's progress.
		if (modern) return {};
		CMsgClientGetUserStatsResponse body;
		body.set_game_id(request.app);
		body.set_eresult(2);
		std::vector<uint8_t> out(body.ByteSizeLong());
		body.SerializeToArray(out.data(), out.size());
		return out;
	};
	if (!g_config.achievements.get() || !request.epoch || size > maxReplyBytes ||
	    header.ByteSizeLong() > maxHeaderBytes ||
	    StatsPolicy::localEpoch(request.app, request.account, true) != request.epoch ||
	    (header.has_steamid() && !StatsPolicy::isSelf(header.steamid(), request.account)))
		return failure();
	if (modern)
	{
		if (!PlayerStats::detail::walk(data, size, [](const auto&) {}) ||
		    PlayerStats::fieldBytes(data, size, 3).empty()) return failure();
		std::vector<uint8_t> out;
		PlayerStats::detail::walk(data, size, [&](const auto& field) {
			if (field.number != 2 && field.number != 4)
				out.insert(out.end(), data + field.start, data + field.end);
		});
		return out;
	}
	CMsgClientGetUserStatsResponse body;
	if (!body.ParseFromArray(data, size) || body.game_id() != request.app ||
	    !body.has_schema() || body.schema().empty()) return failure();
	body.clear_stats();
	body.clear_achievement_blocks();
	body.clear_crc_stats();
	body.set_eresult(ERESULT_OK);
	std::vector<uint8_t> out(body.ByteSizeLong());
	if (!body.SerializeToArray(out.data(), out.size())) return failure();
	return out;
}

void Achievements::recvMessage(const CProtoBufMsgBase* msg)
{
	if (!msg) return;
	// ClientLogOnResponse=751, ClientLoggedOff=757, ClientLicenseList=780.
	if (msg->type != 751 && msg->type != 757 && msg->type != 780) return;
	StatsPolicy::invalidate();
	if (msg->type == 757) StatsPolicy::setAccount(0);
	else if (msg->header && msg->header->has_steamid())
	{
		const auto sid = msg->header->steamid();
		const auto account = static_cast<uint32_t>(sid);
		if (sid && StatsPolicy::isSelf(sid, account)) StatsPolicy::setAccount(account);
	}
	// Keep bounded outstanding jobs so their late replies are rejected rather
	// than allowing the previously selected owner's progress through unchanged.
}
