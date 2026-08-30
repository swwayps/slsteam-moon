#pragma once

#include <cstdint>
#include <string_view>

namespace FamilyShare
{
	constexpr uint32_t SERVICE_METHOD = 146;
	constexpr uint32_t SHARED_LIBRARY_STOP_PLAYING = 9406;
	constexpr std::string_view NOTIFY_RUNNING_APPS = "FamilyGroupsClient.NotifyRunningApps#1";

	constexpr bool shouldChokeIncoming(bool enabled, uint32_t message, std::string_view targetJob) noexcept
	{
		return enabled
			&& (message == SHARED_LIBRARY_STOP_PLAYING
				|| (message == SERVICE_METHOD && targetJob == NOTIFY_RUNNING_APPS));
	}
}
