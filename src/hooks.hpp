#pragma once

#include "sdk/types.hpp"

#include "libmem/libmem.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

class CUtlBuffer;

template<typename T>
class CUtlVector;

struct AppOwnershipInfo_t;
struct AppStateInfo_t;
struct DepotInfo_t;
struct gameserverdetails_t;
struct GamePlayed_t;

struct Pattern_t;
struct VFTableInfo_t;

struct CNetPacket;
enum EWebSocketOpCode : uint32_t;

template<typename T>
union FunctionUnion_t
{
	T fn;
	lm_address_t address;
};

template<typename T>
class Hook
{
public:
	std::string name;
	FunctionUnion_t<T> originalFn;
	FunctionUnion_t<T> hookFn;

	Hook(const char* name);

	// False when the hook could not be placed.
	virtual bool place() = 0;
	virtual void remove() = 0;
};

template<typename T>
class DetourHook : public Hook<T>
{
public:
	FunctionUnion_t<T> tramp;
	size_t size;

	DetourHook(const char* name);
	DetourHook();

	virtual bool place();
	virtual void remove();

	bool setup(const char* name, lm_address_t fn, T hookFn);
	bool setup(const Pattern_t& pattern, T hookFn);
	bool setup(const VFTableInfo_t& info, T hookFn);
};

template<typename T>
class VFTHook : public Hook<T>
{
public:
	std::shared_ptr<lm_vmt_t> vft;
	unsigned int index;
	bool hooked;

	VFTHook(const char* name);
	VFTHook();

	virtual bool place();
	virtual void remove();

	void setup(std::shared_ptr<lm_vmt_t> vft, const VFTableInfo_t& info, T hookFn);
};

// Lua plugin hooks (upstream): a code detour a plugin places and removes
// itself. Each instance registers under a free index for place_lua_hook().
// Only Lua code touches these, always under Lua::stateMutex.
class LuaHook
{
public:
	static std::unordered_map<int, LuaHook*> hooks;

	std::string name = "";
	lm_address_t fn = LM_ADDRESS_BAD;
	lm_address_t hookFn = LM_ADDRESS_BAD;
	lm_address_t tramp = 0;
	size_t size = 0;
	int idx = -1;

	LuaHook(const char* name, void* targetFn);
	~LuaHook();

	void* place();
	bool remove();
};

namespace Hooks
{
	typedef void(*TraceIPC_t)(const char*, const char*);



	typedef void(*CCMInterface_RecvPkt_t)(void*, CNetPacket*);

	typedef uint32_t(*CSteamEngine_ProcessIPCFrame_t)(void*, HSteamPipe, CUtlBuffer*, CUtlBuffer*);
	typedef uint32_t(*CSteamEngine_SetAppIdForCurrentPipe_t)(void*, uint32_t, bool);

	typedef gameserverdetails_t*(*CSteamMatchmakingServers_GetServerDetails_t)(void*, HServerRequest, uint32_t);
	typedef HServerRequest(*CSteamMatchmakingServers_RequestInternetServerList_t)(void*, AppId_t, void*, uint32_t, void*);

	typedef bool(*CUser_CheckAppOwnership_t)(void*, uint32_t, AppOwnershipInfo_t*);
	typedef uint32_t(*CUser_GetSubscribedApps_t)(void*, uint32_t*, uint32_t, uint8_t);
	typedef uint32_t(*CUser_PostCallbackToAppId_t)(void*, uint32_t, uint32_t, void*, uint32_t);
	typedef uint32_t(*CUser_SpawnGameId_t)(void*, const char*, const char*, const char*, GameId_t*, const char*, int32_t, int32_t, int32_t, int32_t, void*);
	typedef uint32_t(*IClientFriends_GetFriendGamePlayed_t)(void*, uint64_t, GamePlayed_t*);

	typedef bool(*CUserAppManager_BuildDepotDependency_t)(void*, uint32_t, void*, CUtlVector<DepotInfo_t>*, CUtlVector<DepotInfo_t>*, void*, uint32_t*, bool*);

	typedef bool(*IClientAppManager_BCanRemotePlayTogether_t)(void*, uint32_t);

	typedef bool(*IClientUser_BLoggedOn_t)(void*);
	typedef uint32_t(*IClientUser_BUpdateAppOwnershipTicket_t)(void*, uint32_t, bool);
	typedef uint32_t(*IClientUser_GetAppOwnershipTicketExtendedData_t)(void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
	typedef bool(*IClientUser_GetEncryptedAppTicket_t)(void*, void*, uint32_t, uint32_t*);
	typedef bool(*IClientUser_GetLegacyCDKey_t)(void*, uint32_t, char*, uint32_t);
	typedef CSteamId(*IClientUser_GetSteamId_t)(void*);
	typedef uint8_t(*IClientUser_IsUserSubscribedAppInTicket_t)(void*, uint64_t, uint32_t);

	typedef bool(*IClientUtils_GetOfflineMode_t)(void*);

	typedef bool(*CWebSocketConnection_BBuildAndAsyncSendFrame_t)(void*, EWebSocketOpCode, uint8_t*, uint32_t);

	typedef bool(*CJobMgr_BRouteMsgToJob_t)(void*, void*, void*, void*);

	typedef bool(*CDepotDownloadMgr_BYldRequestDepotManifest_t)(void*, uint32_t, uint32_t, uint64_t, const char*, void*);

	extern DetourHook<TraceIPC_t> TraceIPC;


	extern DetourHook<CCMInterface_RecvPkt_t> CCMInterface_RecvPkt;

	extern DetourHook<CWebSocketConnection_BBuildAndAsyncSendFrame_t> CWebSocketConnection_BBuildAndAsyncSendFrame;
	extern DetourHook<CJobMgr_BRouteMsgToJob_t> CJobMgr_BRouteMsgToJob;
	extern DetourHook<CDepotDownloadMgr_BYldRequestDepotManifest_t> CDepotDownloadMgr_BYldRequestDepotManifest;

	extern DetourHook<CSteamMatchmakingServers_GetServerDetails_t> CSteamMatchmakingServers_GetServerDetails;
	extern DetourHook<CSteamMatchmakingServers_RequestInternetServerList_t> CSteamMatchmakingServers_RequestInternetServerList;


	extern DetourHook<CSteamEngine_ProcessIPCFrame_t> CSteamEngine_ProcessIPCFrame;
	extern DetourHook<CSteamEngine_SetAppIdForCurrentPipe_t> CSteamEngine_SetAppIdForCurrentPipe;

	extern DetourHook<CUser_CheckAppOwnership_t> CUser_CheckAppOwnership;
	extern DetourHook<CUser_GetSubscribedApps_t> CUser_GetSubscribedApps;
	extern DetourHook<CUser_PostCallbackToAppId_t> CUser_PostCallbackToAppId;
	extern DetourHook<CUser_SpawnGameId_t> CUser_SpawnGameId;

	extern DetourHook<IClientFriends_GetFriendGamePlayed_t> IClientFriends_GetFriendGamePlayed;

	extern DetourHook<CUserAppManager_BuildDepotDependency_t> CUserAppManager_BuildDepotDependency;


	typedef bool(*IClientAppManager_BIsDlcEnabled_t)(void*, uint32_t, uint32_t, void*);
	typedef bool(*IClientAppManager_GetAppUpdateInfo_t)(void*, uint32_t, uint32_t*);
	typedef bool(*IClientAppManager_GetAppStateInfo_t)(void*, uint32_t, AppStateInfo_t*);
	typedef void*(*IClientAppManager_LaunchApp_t)(void*, GameId_t*, uint32_t, uint32_t, const char*);
	typedef bool(*IClientAppManager_IsAppDlcInstalled_t)(void*, uint32_t, uint32_t);
	typedef unsigned int(*IClientApps_GetDLCCount_t)(void*, uint32_t);
	typedef bool(*IClientApps_GetDLCDataByIndex_t)(void*, uint32_t, uint32_t, uint32_t*, bool*, char*, steamsize_t);
	typedef bool(*IClientRemoteStorage_IsCloudEnabledForApp_t)(void*, uint32_t);
	typedef bool(*IClientCompat_BIsCompatLayerEnabled_t)(void*);
	typedef bool(*IClientConfigStore_SetString_t)(void*, uint32_t, const char*, const char*);
	typedef uint32_t(*IClientUtils_GetAppId_t)(void*);

	extern VFTHook<IClientAppManager_BCanRemotePlayTogether_t> IClientAppManager_BCanRemotePlayTogether;
	extern VFTHook<IClientAppManager_BIsDlcEnabled_t> IClientAppManager_BIsDlcEnabled;
	extern VFTHook<IClientAppManager_GetAppUpdateInfo_t> IClientAppManager_GetAppUpdateInfo;
	extern VFTHook<IClientAppManager_GetAppStateInfo_t> IClientAppManager_GetAppStateInfo;
	extern VFTHook<IClientAppManager_LaunchApp_t> IClientAppManager_LaunchApp;
	extern VFTHook<IClientAppManager_IsAppDlcInstalled_t> IClientAppManager_IsAppDlcInstalled;

	extern VFTHook<IClientApps_GetDLCDataByIndex_t> IClientApps_GetDLCDataByIndex;
	extern VFTHook<IClientApps_GetDLCCount_t> IClientApps_GetDLCCount;

	extern DetourHook<IClientRemoteStorage_IsCloudEnabledForApp_t> IClientRemoteStorage_IsCloudEnabledForApp;
	extern DetourHook<IClientCompat_BIsCompatLayerEnabled_t> IClientCompat_BIsCompatLayerEnabled;
	extern DetourHook<IClientConfigStore_SetString_t> IClientConfigStore_SetString;

	extern VFTHook<IClientUser_BLoggedOn_t> IClientUser_BLoggedOn;
	extern VFTHook<IClientUser_BUpdateAppOwnershipTicket_t> IClientUser_BUpdateAppOwnershipTicket;
	extern VFTHook<IClientUser_GetAppOwnershipTicketExtendedData_t> IClientUser_GetAppOwnershipTicketExtendedData;
	extern VFTHook<IClientUser_GetEncryptedAppTicket_t> IClientUser_GetEncryptedAppTicket;
	extern VFTHook<IClientUser_GetLegacyCDKey_t> IClientUser_GetLegacyCDKey;
	extern VFTHook<IClientUser_GetSteamId_t> IClientUser_GetSteamId;
	extern VFTHook<IClientUser_IsUserSubscribedAppInTicket_t> IClientUser_IsUserSubscribedAppInTicket;

	extern VFTHook<IClientUtils_GetAppId_t> IClientUtils_GetAppId;
	extern VFTHook<IClientUtils_GetOfflineMode_t> IClientUtils_GetOfflineMode;

	typedef void(*CGameInfoDialog_ServerResponded_t)(void*, gameserverdetails_t*);


	extern DetourHook<CGameInfoDialog_ServerResponded_t> CGameInfoDialog_ServerResponded;
	bool setup();
	// False when a required hook could not be placed.
	bool place();
	void remove();
	// Installs the interface vtable hooks once the local user exists; called
	// from ProcessIPCFrame on the IPC thread.
	void placeVFTHooks();
}
