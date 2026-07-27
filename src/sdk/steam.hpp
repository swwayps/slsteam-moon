#pragma once

#include <cstdint>
#include <string>


typedef uint32_t AppId_t;
typedef uint32_t EMsg;
typedef uint64_t GameId_t;

typedef int32_t ENetPacket;

typedef uint32_t HSteamPipe;
typedef uint32_t HSteamUser;

constexpr static ENetPacket INVALID_NETPACKET_TYPE = -1;
constexpr static ENetPacket PROTOBUF_TYPE_MASK = 0x80000000;

enum class EIPCCmd : uint8_t
{
	RunInterface = 1,
	SerializeCallbacks = 2,
	CreateGlobalUser = 3,
	DisconnectGlobalUser = 4,
	ClosePipe = 5,
	Heartbeat = 6,
	ConnectPipe = 9
};

std::string EIPCCmd_ToString(const EIPCCmd cmd);

enum class EIPCExitCode : uint8_t
{
	Success = 0xb
};

enum class EIPCInterface : uint8_t
{
	User = 0x1,
	GameServerInternal = 0x2,
	Friends = 0x3,
	Utils = 0x4,
	Billing = 0x5,
	Matchmaking = 0x6,
	Apps = 0x8,
	UserStats = 0xb,
	Networking = 0xc,
	RemoteStorage = 0xd,
	DepotBuilder = 0x10,
	AppManager = 0x11,
	ConfigStore = 0x12,
	GameCoordinator = 0x13,
	GameServerStats = 0x14,
	GameStats = 0x15,
	HTTP = 0x16,
	Screenshots = 0x17,
	Audio = 0x18,
	UnifiedMessages = 0x19,
	StreamLauncher = 0x1a,
	ParentalSettings = 0x1b,
	NetworkDeviceManager = 0x1d,
	Music = 0x1e,
	RemoteClientManager = 0x1f,
	UGC = 0x20,
	StreamClient = 0x21,
	ProductBuilder = 0x22,
	Shortcuts = 0x23,
	GameNotifications = 0x25,
	Video = 0x26,
	Inventory = 0x27,
	VR = 0x28,
	ControllerSerialized = 0x29,
	AppDisableUpdate = 0x2a,
	SharedConnection = 0x2c,
	Shader = 0x2d,
	NetworkingSocketsSerialized = 0x2e,
	Compat = 0x30,
	Parties = 0x31,
	NetworkingUtilsSerialized = 0x32,
	RemotePlay = 0x34,
	GameServerPacketHandler = 0x35,
	SystemManager = 0x36,
	SystemPerfManager = 0x39,
	SystemDockManager = 0x3a,
	SystemAudioManager = 0x3b,
	SystemDisplayManager = 0x3c,
	Timeline = 0x3d
};

std::string EIPCInterface_ToString(const EIPCInterface interface);

namespace Steam
{
	typedef void*(*Plat_Alloc_t)(int);
	typedef void(*Plat_Free_t)(void*);
	typedef void*(*Plat_Realloc_t)(void*, int);

	extern Plat_Alloc_t Plat_Alloc;
	extern Plat_Free_t Plat_Free;
	extern Plat_Realloc_t Plat_Realloc;

	template<typename T>
	T* alloc(int size)
	{
		return reinterpret_cast<T*>(Plat_Alloc(size));
	}

	void free(void* mem);

	template<typename T>
	T* realloc(void* mem, int size)
	{
		return reinterpret_cast<T*>(Plat_Realloc(mem, size));
	}

	bool init();
}
