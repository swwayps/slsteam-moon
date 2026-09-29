#include "achievements.hpp"
#include "stats_policy.hpp"
#include "playerstats.hpp"
#include "../config.hpp"
#include "../curl.hpp"
#include "../log.hpp"
#include "../sdk/CAPIJob.hpp"
#include "../sdk/CClientUnifiedServiceTransport.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/types.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>

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
	body.set_eresult(k_EResultOK);
	std::vector<uint8_t> out(body.ByteSizeLong());
	if (!body.SerializeToArray(out.data(), out.size())) return failure();
	return out;
}

void Achievements::recvMessage(uint32_t type, bool hasSteamId, uint64_t steamId)
{
	// ClientLogOnResponse=751, ClientLoggedOff=757, ClientLicenseList=780.
	if (type != 751 && type != 757 && type != 780) return;
	StatsPolicy::invalidate();
	if (type == 757) StatsPolicy::setAccount(0);
	else if (hasSteamId)
	{
		const auto account = static_cast<uint32_t>(steamId);
		if (steamId && StatsPolicy::isSelf(steamId, account)) StatsPolicy::setAccount(account);
	}
	// Keep bounded outstanding jobs so their late replies are rejected rather
	// than allowing the previously selected owner's progress through unchanged.
}

std::unordered_map<AppId_t, uint64_t> Achievements::preferredOwners;
std::unordered_map<AppId_t, std::unordered_set<uint64_t>> Achievements::ownerBlacklist;

std::string Achievements::getReviewUrl(const AppId_t appId)
{
	std::ostringstream url;
	url << "https://store.steampowered.com/appreviews/" << appId
		<< "?json=1&filter=recent&language=all&purchase_type=all&num_per_page="
		<< g_config.maxSchemaTries.get();

	return url.str();
}

std::unordered_set<uint64_t> Achievements::getReviewersForGame(const AppId_t appId)
{
	auto list = std::unordered_set<uint64_t>();
	if (!g_config.maxSchemaTries.get())
	{
		return list;
	}

	auto url = getReviewUrl(appId);
	std::string reviews;

	if (Curl::getString(url.c_str(), reviews))
	{
		LOG_WARN("Failed to get reviewer list for %u!\n", appId);
		return list;
	}

	if (!ownerBlacklist.contains(appId))
	{
		ownerBlacklist[appId] = std::unordered_set<uint64_t>();
	}

	//LOG_DEBUG("Downloaded reviewers %s\n", reviews.c_str());

	std::regex steamIdFieldsRe("\"steamid\":\"[0-9]+\"");

	auto begin = std::sregex_iterator(reviews.begin(), reviews.end(), steamIdFieldsRe);
	auto end = std::sregex_iterator();

	for (auto i = begin; i != end; ++i)
	{
		std::smatch steamIdMatch = *i;
		std::string steamIdFieldStr = steamIdMatch.str();

		//LOG_DEBUG("SteamId match %s\n", match.str().c_str());

		std::regex idRe("[0-9]+");
		if (!std::regex_search(steamIdFieldStr, steamIdMatch, idRe))
		{
			continue;
		}

		LOG_DEBUG("Extracted SteamId %s\n", steamIdMatch.str().c_str());

		uint64_t steamId = std::stoull(steamIdMatch.str().c_str());
		if (ownerBlacklist[appId].contains(steamId))
		{
			LOG_DEBUG("Skipping %llu for %u because it has failed before\n", steamId, appId);
			continue;
		}

		list.emplace(steamId);
	}

	return list;
}


uint32_t Achievements::tryGetPlayerStats
(
	CClientUnifiedServiceTransport* serviceTransport,
	const char* serviceName,
	CPlayer_GetUserStats_Request* send,
	CPlayer_GetUserStats_Response* recv,
	const uint64_t steamId
)
{
	LOG_DEBUG("CPlayer_GetUserStats_Request->set_steamid(%llu)\n", steamId);
	send->set_steamid(steamId);
	uint32_t res = serviceTransport->sendAndRecvMsg(serviceName, send, recv);

	const AppId_t appId = send->appid();

	if (res == k_EResultFailure)
	{
		ownerBlacklist[appId].emplace(steamId);
		return k_EResultNoResult;
	}
	else if (res != k_EResultOK)
	{
		return k_EResultNoResult;
	}

	recv->clear_crc_stats();
	recv->clear_stats();

	preferredOwners[appId] = steamId;
	LOG_DEBUG("Using steamId %llu for %u\n", steamId, appId);
	return k_EResultOK;
}

uint32_t Achievements::sendAndRecvGetPlayerStats
(
	CClientUnifiedServiceTransport* serviceTransport,
	const char* serviceName,
	CPlayer_GetUserStats_Request* send,
	CPlayer_GetUserStats_Response* recv
)
{
	if (strcmp(serviceName, GET_PLAYER_STATS_SERVICE_NAME) != 0)
	{
		return k_EResultNoResult;
	}

	//Don't do anything for legit apps
	if (g_pSteamEngine->getUser(0)->isSubscribed(send->appid()))
	{
		return k_EResultNoResult;
	}

	const AppId_t appId = send->appid();
	send->clear_crc_stats();

	//Prefer last successfull owner to skip review fetch. Fixes stutters caused
	//by review fetching in games that spam stat requests
	if (preferredOwners.contains(appId))
	{
		const uint32_t res = tryGetPlayerStats(serviceTransport, GET_PLAYER_STATS_SERVICE_NAME, send, recv, preferredOwners.at(appId));
		if (res == k_EResultOK)
		{
			return res;
		}

		preferredOwners.erase(send->appid());
	}

	const auto reviewers = getReviewersForGame(send->appid());

	for (const auto& id : reviewers)
	{
		const uint32_t res = tryGetPlayerStats(serviceTransport, serviceName, send, recv, id);
		if (res == k_EResultOK)
		{
			return res;
		}
	}

	LOG_DEBUG("No schemas for %u found! Falling back to offline cache\n", appId);
	return k_EResultNoConnection;
}

uint32_t Achievements::tryGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType, const uint64_t steamId)
{
	const auto sendBdy = send->getBody<CMsgClientGetUserStats>();

	LOG_DEBUG("CMsgClientGetUserStats->set_steam_id_for_user(%llu)\n", steamId);
	sendBdy->set_steam_id_for_user(steamId);

	const uint32_t ret = job->sendAndRecv(send, timeOut, recv, targetType);
	if (!ret)
	{
		return 0;
	}

	const AppId_t appId = sendBdy->game_id();
	const auto recvBdy = recv->getBody<CMsgClientGetUserStatsResponse>();

	if (recvBdy->eresult() == k_EResultFailure)
	{
		ownerBlacklist[appId].emplace(steamId);
		return 0;
	}

	if (recvBdy->eresult() != k_EResultOK)
	{
		return 0;
	}

	recvBdy->clear_achievement_blocks();
	recvBdy->clear_crc_stats();
	recvBdy->clear_stats();

	LOG_DEBUG("Using steamId %llu for %u\n", steamId, appId);
	preferredOwners[appId] = steamId;
	return ret;
}

uint32_t Achievements::sendAndRecvGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType)
{
	if (targetType != k_EMsgClientGetUserStatsResponse)
	{
		return 0;
	}

	const auto sendBdy = send->getBody<CMsgClientGetUserStats>();

	if (g_pSteamEngine->getUser(0)->isSubscribed(sendBdy->game_id()))
	{
		return 0;
	}

	const AppId_t appId = sendBdy->game_id();
	sendBdy->clear_crc_stats();

	if (preferredOwners.contains(appId))
	{
		uint32_t ret = tryGetUserStats(job, send, timeOut, recv, targetType, preferredOwners.at(appId));
		if (ret)
		{
			return ret;
		}

		//Prefered owner failed, erase then fallthrough to trying reviewers
		preferredOwners.erase(appId);
	}

	const auto reviewers = getReviewersForGame(sendBdy->game_id());

	for (const auto& id : reviewers)
	{
		const uint32_t ret = tryGetUserStats(job, send, timeOut, recv, targetType, id);
		if (ret)
		{
			return ret;
		}
	}

	const auto recvBdy = recv->getBody<CMsgClientGetUserStatsResponse>();
	recvBdy->set_eresult(k_EResultNoConnection);
	LOG_DEBUG("No schemas for %u found! Falling back to offline cache\n", appId);
	return 1;
}
