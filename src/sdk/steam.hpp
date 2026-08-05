#pragma once

#include <cstdint>

typedef uint32_t AppId_t;
typedef uint32_t EMsg;
typedef uint64_t GameId_t;

typedef int32_t ENetPacket;

typedef uint32_t HSteamPipe;
typedef uint32_t HSteamUser;

constexpr static ENetPacket INVALID_NETPACKET_TYPE = -1;
constexpr static ENetPacket PROTOBUF_TYPE_MASK = 0x80000000;

constexpr static HSteamPipe g_globalSteamPipe = 2;
constexpr static HSteamPipe g_globalSteamUser = 1;

enum class EIPCCmd : uint8_t
{
	RunInterface = 1,
	SerializeCallbacks = 2,
	CreateGlobalUser = 3,
	ConnectPipe = 9
};

enum EInterfaceType : uint8_t
{
	k_EInterfaceTypeClientUser = 0x1,
	k_EInterfaceTypeClientGameServerInternal = 0x2,
	k_EInterfaceTypeClientFriends = 0x3,
	k_EInterfaceTypeClientUtils = 0x4,
	k_EInterfaceTypeClientBilling = 0x5,
	k_EInterfaceTypeClientMatchmaking = 0x6,
	k_EInterfaceTypeClientApps = 0x8,
	k_EInterfaceTypeClientUserStats = 0xb,
	k_EInterfaceTypeClientNetworking = 0xc,
	k_EInterfaceTypeClientRemoteStorage = 0xd,
	k_EInterfaceTypeClientDepotBuilder = 0x10,
	k_EInterfaceTypeClientAppManager = 0x11,
	k_EInterfaceTypeClientConfigStore = 0x12,
	k_EInterfaceTypeClientGameCoordinator = 0x13,
	k_EInterfaceTypeClientGameServerStats = 0x14,
	k_EInterfaceTypeClientGameStats = 0x15,
	k_EInterfaceTypeClientHTTP = 0x16,
	k_EInterfaceTypeClientScreenshots = 0x17,
	k_EInterfaceTypeClientAudio = 0x18,
	k_EInterfaceTypeClientUnifiedMessages = 0x19,
	k_EInterfaceTypeClientStreamLauncher = 0x1a,
	k_EInterfaceTypeClientParentalSettings = 0x1b,
	k_EInterfaceTypeClientNetworkDeviceManager = 0x1d,
	k_EInterfaceTypeClientMusic = 0x1e,
	k_EInterfaceTypeClientRemoteClientManager = 0x1f,
	k_EInterfaceTypeClientUGC = 0x20,
	k_EInterfaceTypeClientStreamClient = 0x21,
	k_EInterfaceTypeClientProductBuilder = 0x22,
	k_EInterfaceTypeClientShortcuts = 0x23,
	k_EInterfaceTypeClientGameNotifications = 0x25,
	k_EInterfaceTypeClientVideo = 0x26,
	k_EInterfaceTypeClientInventory = 0x27,
	k_EInterfaceTypeClientVR = 0x28,
	k_EInterfaceTypeClientControllerSerialized = 0x29,
	k_EInterfaceTypeClientAppDisableUpdate = 0x2a,
	k_EInterfaceTypeClientSharedConnection = 0x2c,
	k_EInterfaceTypeClientShader = 0x2d,
	k_EInterfaceTypeClientNetworkingSocketsSerialized = 0x2e,
	k_EInterfaceTypeClientCompat = 0x30,
	k_EInterfaceTypeClientParties = 0x31,
	k_EInterfaceTypeClientNetworkingUtilsSerialized = 0x32,
	k_EInterfaceTypeClientRemotePlay = 0x34,
	k_EInterfaceTypeClientGameServerPacketHandler = 0x35,
	k_EInterfaceTypeClientSystemManager = 0x36,
	k_EInterfaceTypeClientSystemPerfManager = 0x39,
	k_EInterfaceTypeClientSystemDockManager = 0x3a,
	k_EInterfaceTypeClientSystemAudioManager = 0x3b,
	k_EInterfaceTypeClientSystemDisplayManager = 0x3c,
	k_EInterfaceTypeClientTimeline = 0x3d
};

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
