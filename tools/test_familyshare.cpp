// Regression coverage for the incoming Family Share packet filter.
#include "feats/familyshare.hpp"
#include "sdk/CNetPacket.hpp"
#include "sdk/steam.hpp"

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

	CMsgProtoBufHeader sourceHeader;
	sourceHeader.set_target_job_name(std::string(kNotifyRunningApps));
	std::string serializedHeader;
	expect(
		sourceHeader.SerializeToString(&serializedHeader),
		"test header serializes");
	std::vector<std::uint8_t> validStorage(
		sizeof(CNetPacketBody) + serializedHeader.size());
	auto* validBody = reinterpret_cast<CNetPacketBody*>(validStorage.data());
	validBody->type = CNetPacket::PROTOBUF_TYPE_MASK | kServiceMethod;
	validBody->headerSize = serializedHeader.size();
	std::memcpy(
		validStorage.data() + sizeof(CNetPacketBody),
		serializedHeader.data(),
		serializedHeader.size());
	CNetPacket validPacket{};
	validPacket.body = validBody;
	validPacket.size = validStorage.size();
	CMsgProtoBufHeader parsedHeader;
	expect(
		validPacket.deserializeHeader(parsedHeader)
			&& parsedHeader.has_target_job_name()
			&& parsedHeader.target_job_name() == kNotifyRunningApps,
		"valid packet header parses within bounds");

	CNetPacket nullPacket{};
	expect(
		!nullPacket.deserializeHeader(parsedHeader),
		"null packet header is rejected");

	std::uint8_t invalidStorage[sizeof(CNetPacketBody) + 1]{};
	auto* invalidBody = reinterpret_cast<CNetPacketBody*>(invalidStorage);
	invalidBody->type = CNetPacket::PROTOBUF_TYPE_MASK | kServiceMethod;
	CNetPacket invalidPacket{};
	invalidPacket.body = invalidBody;
	invalidPacket.size = sizeof(CNetPacketBody) - 1;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"undersized packet header is rejected");

	invalidPacket.size = sizeof(invalidStorage);
	invalidBody->headerSize = 2;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"header larger than packet payload is rejected");

	invalidPacket.size = std::numeric_limits<std::uint32_t>::max();
	invalidBody->headerSize =
		static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1u;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"header larger than protobuf parser range is rejected");

	invalidPacket.size = sizeof(invalidStorage);
	invalidBody->headerSize = 1;
	invalidStorage[sizeof(CNetPacketBody)] = 0x80;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"truncated protobuf header is rejected");

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
