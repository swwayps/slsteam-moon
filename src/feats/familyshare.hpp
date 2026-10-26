#pragma once

#include "../sdk/CNetPacket.hpp"

#include <cstdint>
#include <string_view>

namespace FamilyShare
{
inline constexpr std::uint32_t kServiceMethod = 146;
inline constexpr std::uint32_t kSharedLibraryStopPlaying = 9406;
inline constexpr std::string_view kNotifyRunningApps =
	"FamilyGroupsClient.NotifyRunningApps#1";

inline bool shouldBlock(
	bool enabled,
	std::uint32_t protobufType,
	bool hasTargetJobName,
	std::string_view targetJobName)
{
	return enabled
	    && (protobufType == kSharedLibraryStopPlaying
	        || (protobufType == kServiceMethod
	            && hasTargetJobName
	            && targetJobName == kNotifyRunningApps));
}

// The CM receive hook's decision for one protobuf packet. Every CM packet
// passes here and only the two family types can be blocked, so only those
// have their header parsed.
inline bool shouldChoke(bool enabled, const CNetPacket& packet)
{
	const std::uint32_t type = packet.getProtoBufType();
	if (!enabled || (type != kServiceMethod && type != kSharedLibraryStopPlaying))
		return false;
	CMsgProtoBufHeader header;
	if (!packet.deserializeHeader(header)) return false;
	const bool hasTargetJobName = header.has_target_job_name();
	return shouldBlock(
		enabled, type, hasTargetJobName,
		hasTargetJobName ? std::string_view(header.target_job_name())
		                 : std::string_view{});
}
} // namespace FamilyShare
