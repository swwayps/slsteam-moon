// Regression coverage for the incoming Family Share packet filter.
#include "feats/familyshare.hpp"
#include "sdk/CNetPacket.hpp"
#include "sdk/steam.hpp"

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <new>
#include <vector>

namespace
{
bool g_countAllocations = false;
std::size_t g_allocations = 0;
}

void* operator new(std::size_t size)
{
	if (g_countAllocations) ++g_allocations;
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

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

void* fakePlatAlloc(int size)
{
	return std::malloc(static_cast<std::size_t>(size));
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
Plat_Alloc_t Plat_Alloc = fakePlatAlloc;
Plat_Free_t Plat_Free = fakePlatFree;
Plat_Realloc_t Plat_Realloc = nullptr;
}

std::unique_ptr<CLog> g_pLog;
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }

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
	validPacket.setBody(validBody);
	validPacket.setSize(validStorage.size());
	CMsgProtoBufHeader parsedHeader;
	expect(
		validPacket.deserializeHeader(parsedHeader)
			&& parsedHeader.has_target_job_name()
			&& parsedHeader.target_job_name() == kNotifyRunningApps,
		"valid packet header parses within bounds");

	expect(
		FamilyShare::shouldChoke(true, validPacket),
		"the receive hook chokes the running-app service method");

	// Every other CM packet passes through the same check: it must be decided
	// from the type alone, without parsing a header.
	validBody->type = CNetPacket::PROTOBUF_TYPE_MASK | 751;
	g_allocations = 0;
	g_countAllocations = true;
	const bool choked = FamilyShare::shouldChoke(true, validPacket);
	g_countAllocations = false;
	expect(!choked && g_allocations == 0,
	       "an unrelated packet is forwarded without parsing its header");
	validBody->type = CNetPacket::PROTOBUF_TYPE_MASK | kServiceMethod;

	CNetPacket nullPacket{};
	expect(
		!nullPacket.deserializeHeader(parsedHeader),
		"null packet header is rejected");

	std::uint8_t invalidStorage[sizeof(CNetPacketBody) + 1]{};
	auto* invalidBody = reinterpret_cast<CNetPacketBody*>(invalidStorage);
	invalidBody->type = CNetPacket::PROTOBUF_TYPE_MASK | kServiceMethod;
	CNetPacket invalidPacket{};
	invalidPacket.setBody(invalidBody);
	invalidPacket.setSize(sizeof(CNetPacketBody) - 1);
	expect(
		invalidPacket.getType() == CNetPacket::INVALID_MESSAGE_TYPE,
		"undersized packet type is not read");
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"undersized packet header is rejected");

	invalidPacket.setSize(sizeof(invalidStorage));
	invalidBody->headerSize = 2;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"header larger than packet payload is rejected");

	invalidPacket.setSize(std::numeric_limits<std::uint32_t>::max());
	invalidBody->headerSize =
		static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1u;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"header larger than protobuf parser range is rejected");

	invalidPacket.setSize(sizeof(invalidStorage));
	invalidBody->headerSize = 1;
	invalidStorage[sizeof(CNetPacketBody)] = 0x80;
	expect(
		!invalidPacket.deserializeHeader(parsedHeader),
		"truncated protobuf header is rejected");

	const pid_t child = fork();
	if (child == 0)
	{
		const long pageSize = sysconf(_SC_PAGESIZE);
		auto* mapping = static_cast<std::uint8_t*>(mmap(
			nullptr, static_cast<std::size_t>(pageSize) * 2,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (mapping == MAP_FAILED ||
			mprotect(mapping + pageSize, pageSize, PROT_NONE) != 0)
		{
			_exit(2);
		}

		auto* guardedBody = reinterpret_cast<CNetPacketBody*>(
			mapping + pageSize - sizeof(CNetPacketBody));
		guardedBody->type = CNetPacket::PROTOBUF_TYPE_MASK | kServiceMethod;
		guardedBody->headerSize = 1;
		CNetPacket guardedPacket{};
		guardedPacket.setBody(guardedBody);
		guardedPacket.setOriginalBody(guardedBody);
		guardedPacket.setSize(sizeof(CNetPacketBody));
		CMsgProtoBufHeader emptyHeader;
		guardedPacket.serialize(emptyHeader);
		_exit(0);
	}
	int childStatus = 0;
	expect(child > 0 && waitpid(child, &childStatus, 0) == child &&
	       WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0,
	       "serialization rejects a header extending beyond the packet");

	static_assert(sizeof(CNetPacket) == 0x20);
	CNetPacket packet{};
	std::uint8_t body[8]{};
	std::uint8_t originalBody[8]{};
	packet.setBody(reinterpret_cast<CNetPacketBody*>(body));
	packet.setOriginalBody(reinterpret_cast<CNetPacketBody*>(originalBody));
	packet.setSize(sizeof(body));
	packet.free();
	expect(
		freeCalls == 1 && freedBody == body,
		"packet release calls Steam::Plat_Free exactly once for body");
	CNetPacketBody* releasedOriginal = reinterpret_cast<CNetPacketBody*>(1);
	std::memcpy(&releasedOriginal,
		reinterpret_cast<const std::uint8_t*>(&packet) + CNetPacket::OFF_ORIGBODY,
		sizeof(releasedOriginal));
	expect(
		packet.body() == nullptr && releasedOriginal == nullptr && packet.sizeBytes() == 0,
		"packet release clears body ownership fields");

	return failures == 0 ? 0 : 1;
}
