#pragma once

#include "CUtl.hpp"
#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class CUser;
class IClientUtils;

enum class EIPCCmd : uint8_t
{
	RunInterface = 1,
	SerializeCallbacks = 2,
	CreateGlobalUser = 3, //Also used to connect to global user
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

class IProcessPipe { };
using CUtlString = char*;

SDK_Class CServerPipe
{
public:
	IProcessPipe* internalPipe;			//0x0
	IProcessPipe* singleProcessPipe;	//0x8
	HSteamPipe pipeHandle;				//0x10
	uint8_t __pad0xC[0xC];				//0x14
	int32_t pid;						//0x20
	int32_t threadId;					//0x24
	CUtlString processName;				//0x28 - Was empty on the stuff I tried, maybe it's defunct on linux?
};

static_assert(offsetof(CServerPipe, pipeHandle) == 0x10);
static_assert(offsetof(CServerPipe, pid) == 0x20);

SDK_Class CSteamEngine
{
public:
	CServerPipe* getServerPipe(HSteamPipe pipe);
	CUser* getUser(uint32_t index = 0);
	IClientUtils* getUtils();
	void setAppIdForCurrentPipe(uint32_t appId);
};

extern CSteamEngine* g_pSteamEngine;

// Fallback to the local CUser* captured from the CUser::CheckAppOwnership
// hook.  g_pSteamEngine is only captured by the first ProcessIPCFrame;
// CheckAppOwnership fires earlier and often with the same CUser* that
// getUser(0) would return, so it is a reliable, timing-independent source.
// Written from whichever Steam thread runs that hook, read from others.
extern std::atomic<CUser*> g_pLocalUser;

// Returns the local user (pipe 0), preferring the engine when available and
// falling back to the cached CheckAppOwnership user.  May return nullptr if
// neither source has been observed yet; callers MUST null-check.
CUser* getLocalUser();
