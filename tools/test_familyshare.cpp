// Regression coverage for the incoming Family Share packet filter.
#include "feats/familyshare.hpp"
#include "sdk/CNetPacket.hpp"
#include "sdk/steam.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace
{
constexpr std::uint32_t kServiceMethod = 146;
constexpr std::uint32_t kSharedLibraryStopPlaying = 9406;
constexpr std::string_view kNotifyRunningApps =
	"FamilyGroupsClient.NotifyRunningApps#1";

int failures = 0;
unsigned int freeCalls = 0;
void* freedBody = nullptr;

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

void fakePlatFree(void* memory)
{
	++freeCalls;
	freedBody = memory;
}

bool blocks(
	bool enabled,
	std::uint32_t protobufType,
	bool hasTargetJobName,
	std::string_view targetJobName)
{
	return FamilyShare::shouldBlock(
		enabled, protobufType, hasTargetJobName, targetJobName);
}
}

namespace Steam
{
Plat_Alloc_t Plat_Alloc = nullptr;
Plat_Free_t Plat_Free = fakePlatFree;
Plat_Realloc_t Plat_Realloc = nullptr;
}

int main()
{
	expect(
		blocks(true, kSharedLibraryStopPlaying, false, {}),
		"enabled policy blocks SharedLibraryStopPlaying");
	expect(
		blocks(true, kServiceMethod, true, kNotifyRunningApps),
		"enabled policy blocks the FamilyGroups running-app service method");
	expect(
		!blocks(false, kSharedLibraryStopPlaying, false, {}),
		"disabled policy forwards SharedLibraryStopPlaying");
	expect(
		!blocks(false, kServiceMethod, true, kNotifyRunningApps),
		"disabled policy forwards the FamilyGroups running-app service method");
	expect(
		!blocks(true, kServiceMethod, false, kNotifyRunningApps),
		"service method without a target job name forwards");
	expect(
		!blocks(true, kServiceMethod, true, "Player.NotifyLastPlayedTimes#1"),
		"different service target forwards");
	expect(
		!blocks(true, 742, true, kNotifyRunningApps),
		"unrelated protobuf type with the FamilyGroups target forwards");
	expect(
		!blocks(true, 742, false, {}),
		"unrelated protobuf type forwards");
	expect(
		!blocks(true, 0, false, {}),
		"non-protobuf packet facts forward");
	expect(
		!blocks(true, 0xffffffffu, false, {}),
		"invalid packet facts forward");

	bool repeatedBlock = true;
	bool repeatedForward = true;
	for (unsigned int decision = 0; decision < 100; ++decision)
	{
		repeatedBlock &= blocks(true, kSharedLibraryStopPlaying, false, {});
		repeatedForward &= !blocks(true, kServiceMethod, false, {});
	}
	expect(repeatedBlock, "repeated SharedLibraryStopPlaying decisions remain blocked");
	expect(repeatedForward, "repeated unrelated decisions remain forwarded");

	static_assert(sizeof(CNetPacket) == 0x20);
	CNetPacket packet{};
	std::uint8_t body[8]{};
	std::uint8_t originalBody[8]{};
	packet.body = reinterpret_cast<CNetPacketBody*>(body);
	packet.originalBody = reinterpret_cast<CNetPacketBody*>(originalBody);
	packet.size = sizeof(body);
	packet.free();
	expect(
		freeCalls == 1 && freedBody == body,
		"packet release calls Steam::Plat_Free exactly once for body");
	expect(
		packet.body == nullptr && packet.originalBody == nullptr && packet.size == 0,
		"packet release clears body ownership fields");

	return failures == 0 ? 0 : 1;
}
