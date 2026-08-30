#include "feats/familyshare.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>


namespace
{
constexpr std::uint32_t kServiceMethod = 146;
constexpr std::uint32_t kGamesPlayed = 742;
constexpr std::uint32_t kSharedLibraryStopPlaying = 9406;
constexpr std::string_view kNotifyRunningApps =
	"FamilyGroupsClient.NotifyRunningApps#1";

int failures = 0;

void expect(bool condition, const char* message)
{
	if (condition)
	{
		std::printf("ok:   %s\n", message);
		return;
	}

	std::printf("FAIL: %s\n", message);
	++failures;
}

unsigned int forwardedRepeated(
	bool enabled,
	std::uint32_t message,
	std::string_view targetJob
)
{
	unsigned int forwarded = 0;
	for (unsigned int i = 0; i < 100; ++i)
	{
		if (!FamilyShare::shouldChokeIncoming(enabled, message, targetJob))
			++forwarded;
	}
	return forwarded;
}
}

int main()
{
	expect(
		forwardedRepeated(true, kSharedLibraryStopPlaying, {}) == 0,
		"repeated SharedLibraryStopPlaying notifications are fully choked"
	);
	expect(
		forwardedRepeated(true, kServiceMethod, kNotifyRunningApps) == 0,
		"repeated FamilyGroups running-app notifications are fully choked"
	);
	expect(
		forwardedRepeated(true, kServiceMethod, "Player.NotifyLastPlayedTimes#1") == 100,
		"unrelated service notifications pass through"
	);
	expect(
		forwardedRepeated(true, kGamesPlayed, {}) == 100,
		"unrelated protobuf messages pass through"
	);
	expect(
		forwardedRepeated(false, kSharedLibraryStopPlaying, {}) == 100,
		"SharedLibraryStopPlaying passes when the option is disabled"
	);
	expect(
		forwardedRepeated(false, kServiceMethod, kNotifyRunningApps) == 100,
		"FamilyGroups running-app notifications pass when the option is disabled"
	);

	return failures == 0 ? 0 : 1;
}
