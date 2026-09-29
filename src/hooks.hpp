#pragma once

#include "sdk/steam.hpp"
#include "sdk/protobufs/enums_clientserver.pb.h"

#include "libmem/libmem.h"

#include <cstddef>
#include <memory>
#include <string>

class CAPIJob;
class CClientUnifiedServiceTransport;
class CProtoBufMsgBase;
class CUtlBuffer;

template<typename T>
class CUtlVector;

struct AppOwnershipInfo_t;
struct DepotInfo_t;
struct gameserverdetails_t;
struct GamePlayed_t;

struct Pattern_t;
struct VFTableInfo_t;

struct CNetPacket;
struct CRemoteClientPacket;
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

	virtual void place() = 0;
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

	virtual void place();
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

	virtual void place();
	virtual void remove();

	void setup(std::shared_ptr<lm_vmt_t> vft, const VFTableInfo_t& info, T hookFn);
};

namespace Hooks
{
	typedef void(*TraceIPC_t)(const char*, const char*);


	typedef uint32_t(*CAPIJob_GetPlayerStats_t)(void*);
	typedef uint32_t(*CAPIJob_SendAndRecv_t)(CAPIJob*, CProtoBufMsgBase*, uint32_t, uint32_t, CProtoBufMsgBase*, EMsg);
	typedef uint32_t(*CClientUnifiedServiceMethod_SendAndRecvMsg_t)(CClientUnifiedServiceTransport*, const char*, void*, void*, void*);

	typedef void(*CCMInterface_RecvPkt_t)(void*, CNetPacket*);

	typedef uint32_t(*CSteamEngine_ProcessIPCFrame_t)(void*, HSteamPipe, CUtlBuffer*, CUtlBuffer*);
	typedef uint32_t(*CSteamEngine_SetAppIdForCurrentPipe_t)(void*, uint32_t, bool);

	typedef gameserverdetails_t*(*CSteamMatchmakingServers_GetServerDetails_t)(void*, uint32_t, uint32_t);
	typedef uint32_t(*CSteamMatchmakingServers_RequestInternetServerList_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t);

	typedef uint32_t(*CUser_CheckAppOwnership_t)(void*, uint32_t, AppOwnershipInfo_t*);
	typedef uint32_t(*CUser_GetSubscribedApps_t)(void*, uint32_t*, uint32_t, uint8_t);
	typedef uint32_t(*CUser_PostCallbackToAppId_t)(void*, uint32_t, uint32_t, void*, uint32_t);
	typedef uint32_t(*IClientFriends_GetFriendGamePlayed_t)(void*, uint64_t, GamePlayed_t*);

	typedef bool(*CUserAppManager_BuildDepotDependency_t)(void*, uint32_t, void*, CUtlVector<DepotInfo_t>*, CUtlVector<DepotInfo_t>*, void*, uint32_t*, bool*);

	typedef bool(*IClientAppManager_BCanRemotePlayTogether_t)(void*, uint32_t);

	typedef bool(*IClientUser_BLoggedOn_t)(void*);
	typedef uint32_t(*IClientUser_BUpdateAppOwnershipTicket_t)(void*, uint32_t, bool);
	typedef uint32_t(*IClientUser_GetAppOwnershipTicketExtendedData_t)(void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
	typedef bool(*IClientUser_GetEncryptedAppTicket_t)(void*, void*, uint32_t, uint32_t*);
	typedef uint8_t(*IClientUser_IsUserSubscribedAppInTicket_t)(void*, uint64_t, uint32_t);
	typedef bool(*IClientUser_RequiresLegacyCDKey_t)(void*, uint32_t, uint32_t*);

	typedef bool(*IClientUtils_GetOfflineMode_t)(void*);

	typedef bool(*CWebSocketConnection_BBuildAndAsyncSendFrame_t)(void*, EWebSocketOpCode, uint8_t*, uint32_t);
	typedef void*(*CRemoteClientManager_RecvPkt_t)(void*, CRemoteClientPacket*);

	typedef bool(*CJobMgr_BRouteMsgToJob_t)(void*, void*, void*, void*);

	typedef bool(*CDepotDownloadMgr_BYldRequestDepotManifest_t)(void*, uint32_t, uint32_t, uint64_t, const char*, void*);

	extern DetourHook<TraceIPC_t> TraceIPC;

	extern DetourHook<CAPIJob_GetPlayerStats_t> CAPIJob_GetPlayerStats;
	extern DetourHook<CAPIJob_SendAndRecv_t> CAPIJob_SendAndRecv;
	extern DetourHook<CClientUnifiedServiceMethod_SendAndRecvMsg_t> CClientUnifiedServiceMethod_SendAndRecvMsg;

	extern DetourHook<CCMInterface_RecvPkt_t> CCMInterface_RecvPkt;

	extern DetourHook<CWebSocketConnection_BBuildAndAsyncSendFrame_t> CWebSocketConnection_BBuildAndAsyncSendFrame;
	extern DetourHook<CRemoteClientManager_RecvPkt_t> CRemoteClientManager_RecvPkt;
	extern DetourHook<CJobMgr_BRouteMsgToJob_t> CJobMgr_BRouteMsgToJob;
	extern DetourHook<CDepotDownloadMgr_BYldRequestDepotManifest_t> CDepotDownloadMgr_BYldRequestDepotManifest;

	extern DetourHook<CSteamMatchmakingServers_GetServerDetails_t> CSteamMatchmakingServers_GetServerDetails;
	extern DetourHook<CSteamMatchmakingServers_RequestInternetServerList_t> CSteamMatchmakingServers_RequestInternetServerList;


	extern DetourHook<CSteamEngine_ProcessIPCFrame_t> CSteamEngine_ProcessIPCFrame;
	extern DetourHook<CSteamEngine_SetAppIdForCurrentPipe_t> CSteamEngine_SetAppIdForCurrentPipe;

	extern DetourHook<CUser_CheckAppOwnership_t> CUser_CheckAppOwnership;
	extern DetourHook<CUser_GetSubscribedApps_t> CUser_GetSubscribedApps;
	extern DetourHook<CUser_PostCallbackToAppId_t> CUser_PostCallbackToAppId;

	extern DetourHook<IClientFriends_GetFriendGamePlayed_t> IClientFriends_GetFriendGamePlayed;

	extern DetourHook<CUserAppManager_BuildDepotDependency_t> CUserAppManager_BuildDepotDependency;


	typedef bool(*IClientAppManager_BIsDlcEnabled_t)(void*, uint32_t, uint32_t, void*);
	typedef bool(*IClientAppManager_GetAppUpdateInfo_t)(void*, uint32_t, uint32_t*);
	typedef void*(*IClientAppManager_LaunchApp_t)(void*, uint32_t*, void*, void*, void*);
	typedef bool(*IClientAppManager_IsAppDlcInstalled_t)(void*, uint32_t, uint32_t);
	typedef unsigned int(*IClientApps_GetDLCCount_t)(void*, uint32_t);
	typedef bool(*IClientApps_GetDLCDataByIndex_t)(void*, uint32_t, int, uint32_t*, bool*, char*, size_t);
	typedef int32_t(*IClientApps_GetAppData_t)(void*, uint32_t, const char*, char*, uint32_t);
	typedef bool(*IClientRemoteStorage_IsCloudEnabledForApp_t)(void*, uint32_t);
	typedef bool(*IClientConfigStoreMap_SetString_t)(void*, uint32_t, const char*, const char*);
	typedef uint32_t(*IClientUtils_GetAppId_t)(void*);

	extern VFTHook<IClientAppManager_BCanRemotePlayTogether_t> IClientAppManager_BCanRemotePlayTogether;
	extern VFTHook<IClientAppManager_BIsDlcEnabled_t> IClientAppManager_BIsDlcEnabled;
	extern VFTHook<IClientAppManager_GetAppUpdateInfo_t> IClientAppManager_GetAppUpdateInfo;
	extern VFTHook<IClientAppManager_LaunchApp_t> IClientAppManager_LaunchApp;
	extern VFTHook<IClientAppManager_IsAppDlcInstalled_t> IClientAppManager_IsAppDlcInstalled;

	extern VFTHook<IClientApps_GetDLCDataByIndex_t> IClientApps_GetDLCDataByIndex;
	extern VFTHook<IClientApps_GetDLCCount_t> IClientApps_GetDLCCount;
	extern VFTHook<IClientApps_GetAppData_t> IClientApps_GetAppData;

	extern DetourHook<IClientRemoteStorage_IsCloudEnabledForApp_t> IClientRemoteStorage_IsCloudEnabledForApp;
	extern DetourHook<IClientConfigStoreMap_SetString_t> IClientConfigStoreMap_SetString;

	extern VFTHook<IClientUser_BLoggedOn_t> IClientUser_BLoggedOn;
	extern VFTHook<IClientUser_BUpdateAppOwnershipTicket_t> IClientUser_BUpdateAppOwnershipTicket;
	extern VFTHook<IClientUser_GetAppOwnershipTicketExtendedData_t> IClientUser_GetAppOwnershipTicketExtendedData;
	extern VFTHook<IClientUser_GetEncryptedAppTicket_t> IClientUser_GetEncryptedAppTicket;
	extern VFTHook<IClientUser_IsUserSubscribedAppInTicket_t> IClientUser_IsUserSubscribedAppInTicket;
	extern VFTHook<IClientUser_RequiresLegacyCDKey_t> IClientUser_RequiresLegacyCDKey;

	extern VFTHook<IClientUtils_GetAppId_t> IClientUtils_GetAppId;
	extern VFTHook<IClientUtils_GetOfflineMode_t> IClientUtils_GetOfflineMode;

	typedef void(*CGameInfoDialog_ServerResponded_t)(void*, gameserverdetails_t*);


	extern DetourHook<CGameInfoDialog_ServerResponded_t> CGameInfoDialog_ServerResponded;
	bool setup();
	void place();
	void remove();
	// Installs the interface vtable hooks once the local user exists; called
	// from ProcessIPCFrame on the IPC thread.
	void placeVFTHooks();
}
