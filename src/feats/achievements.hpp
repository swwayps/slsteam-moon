#pragma once

#include "../sdk/protobufs/enums_clientserver.pb.h"

#include "../sdk/steam.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


class CAPIJob;
class CClientUnifiedServiceTransport;
class CMsgProtoBufHeader;
class CPlayer_GetUserStats_Request;
class CPlayer_GetUserStats_Response;
class CProtoBufMsgBase;

namespace Achievements
{
inline constexpr uint32_t maxReplyBytes = 262144;
inline constexpr uint32_t maxHeaderBytes = 1024;
inline uint64_t resolveOwnerSteamId(uint32_t app,
	const std::unordered_map<uint32_t, uint64_t>& perApp, uint64_t defaultOwner)
{
	const auto it = perApp.find(app);
	return it != perApp.end() && it->second ? it->second : defaultOwner;
}

// Empty request output / nullopt response means byte-for-byte passthrough.
// A present empty response is a correlated failure with header.eresult=Fail.
// Correlation uses final CM job IDs, never app ID alone.
std::vector<uint8_t> rewriteRequest(bool modern, const uint8_t* data,
	uint32_t size, const CMsgProtoBufHeader& header);
std::optional<std::vector<uint8_t>> rewriteResponse(bool modern, const uint8_t* data,
	uint32_t size, CMsgProtoBufHeader& header);
// Called by the CM-receive dispatch with the login/logoff/license eMsg and
// the parsed header's steamid.
void recvMessage(uint32_t type, bool hasSteamId, uint64_t steamId);

	constexpr const char* GET_PLAYER_STATS_SERVICE_NAME = "Player.GetUserStats#1";

	extern std::unordered_map<AppId_t, uint64_t> preferredOwners;
	extern std::unordered_map<AppId_t, std::unordered_set<uint64_t>> ownerBlacklist;

	std::string getReviewUrl(const AppId_t appId);
	std::unordered_set<uint64_t> getReviewersForGame(const AppId_t appId);

	uint32_t tryGetPlayerStats
	(
		CClientUnifiedServiceTransport* serviceTransport,
		const char* serviceName,
		CPlayer_GetUserStats_Request* send,
		CPlayer_GetUserStats_Response* recv,
		const uint64_t steamId
	);
	//CPlayer_GetUserStats
	uint32_t sendAndRecvGetPlayerStats
	(
		CClientUnifiedServiceTransport* serviceTransport,
		const char* serviceName,
		CPlayer_GetUserStats_Request* send,
		CPlayer_GetUserStats_Response* recv
	);

	uint32_t tryGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType, const uint64_t steamId);
	//GetUserStats
	uint32_t sendAndRecvGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType);
}
