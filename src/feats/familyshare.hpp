#pragma once

#include <cstdint>
#include <string_view>

namespace FamilyShare
{
inline bool shouldBlock(
	bool enabled,
	std::uint32_t protobufType,
	bool hasTargetJobName,
	std::string_view targetJobName)
{
	constexpr std::uint32_t kServiceMethod = 146;
	constexpr std::uint32_t kSharedLibraryStopPlaying = 9406;
	constexpr std::string_view kNotifyRunningApps =
		"FamilyGroupsClient.NotifyRunningApps#1";

	return enabled
	    && (protobufType == kSharedLibraryStopPlaying
	        || (protobufType == kServiceMethod
	            && hasTargetJobName
	            && targetJobName == kNotifyRunningApps));
}
} // namespace FamilyShare
