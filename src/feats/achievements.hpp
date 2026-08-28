#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

class CProtoBufMsgBase;
class CMsgProtoBufHeader;

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
void recvMessage(const CProtoBufMsgBase* msg);
}
