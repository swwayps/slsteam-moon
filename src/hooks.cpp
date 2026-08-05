#include "hooks.hpp"

#include "config.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "memhlp.hpp"
#include "patterns.hpp"
#include "vftableinfo.hpp"

#include "sdk/CAppOwnershipInfo.hpp"
#include "sdk/CProtoBufMsgBase.hpp"
#include "sdk/CSteamEngine.hpp"
#include "sdk/CSteamMatchmakingServers.hpp"
#include "sdk/CUser.hpp"
#include "sdk/EResult.hpp"
#include "sdk/IClientAppManager.hpp"
#include "sdk/IClientApps.hpp"
#include "sdk/IClientUtils.hpp"

#include "feats/achievements.hpp"
#include "feats/stats_policy.hpp"
#include "feats/appinfostate.hpp"
#include "feats/appticket.hpp"
#include "feats/apps.hpp"
#include "feats/depotkey.hpp"
#include "feats/depotquarantine.hpp"
#include "feats/dlc.hpp"
#include "feats/manifestcode.hpp"
#include "feats/manifestbind.hpp"
#include "feats/misc.hpp"
#include "feats/fakeappid.hpp"
#include "feats/libraryremoval.hpp"
#include "feats/packagepatch.hpp"
#include "feats/parental.hpp"
#include "feats/pics.hpp"
#include "feats/reconcilepin.hpp"
#include "feats/steamstub.hpp"
#include "feats/ticket.hpp"
#include "afftrace.hpp"
#include "ownerwork.hpp"
#include "runtime_attestation.hpp"

#include "libmem/libmem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <span>
#include <strings.h>
#include <unistd.h>
#include <vector>


static bool isExecutableAddress(lm_address_t address)
{
	if (address == LM_ADDRESS_BAD)
		return false;
	lm_segment_t segment {};
	return LM_FindSegment(address, &segment)
	    && (segment.prot & LM_PROT_XR) == LM_PROT_XR;
}

static void attestHookInvocation(std::once_flag& once, const std::string& symbol)
{
	if (!RuntimeAttestation::enabled())
		return;
	std::call_once
	(
		once,
		[&symbol]
		{
			RuntimeAttestation::emit
			(
				"hook-invoked",
				{
					RuntimeAttestation::Field::text("locator", symbol),
					RuntimeAttestation::Field::number("count", 1),
				}
			);
		}
	);
}


template<typename T>
Hook<T>::Hook(const char* name)
{
	this->name = std::string(name);
}

template<typename T>
DetourHook<T>::DetourHook(const char* name) : Hook<T>::Hook(name)
{
	this->size = 0;
}

template<typename T>
DetourHook<T>::DetourHook() : DetourHook<T>("")
{

}

template<typename T>
VFTHook<T>::VFTHook(const char* name) : Hook<T>::Hook(name)
{
	this->hooked = false;
}

template<typename T>
bool DetourHook<T>::setup(Pattern_t pattern, T hookFn)
{
	if (pattern.address == LM_ADDRESS_BAD)
	{
		return false;
	}

	this->name = pattern.name;
	this->originalFn.address = pattern.address;
	this->hookFn.fn = hookFn;

	return true;
}

template<typename T>
void DetourHook<T>::place()
{
	this->size = LM_HookCode(this->originalFn.address, this->hookFn.address, &this->tramp.address);
	MemHlp::fixPICThunkCall(this->name.c_str(), this->originalFn.address, this->tramp.address);

	if (RuntimeAttestation::enabled())
	{
		const lm_module_t* targetModule = nullptr;
		const char* moduleName = "unknown";
		if (this->originalFn.address >= g_modSteamClient.base
		    && this->originalFn.address < g_modSteamClient.base + g_modSteamClient.size)
		{
			targetModule = &g_modSteamClient;
			moduleName = "steamclient";
		}
		else if (this->originalFn.address >= g_modSteamUI.base
		         && this->originalFn.address < g_modSteamUI.base + g_modSteamUI.size)
		{
			targetModule = &g_modSteamUI;
			moduleName = "steamui";
		}

		const bool targetExecutable = isExecutableAddress(this->originalFn.address);
		const bool trampolineExecutable = isExecutableAddress(this->tramp.address);
		RuntimeAttestation::emit
		(
			"hook-installed",
			{
				RuntimeAttestation::Field::text("locator", this->name),
				RuntimeAttestation::Field::text("install_kind", "detour"),
				RuntimeAttestation::Field::text("module", moduleName),
				RuntimeAttestation::Field::number
				(
					"target_rva",
					targetModule ? this->originalFn.address - targetModule->base : 0
				),
				RuntimeAttestation::Field::number("patch_size", this->size),
				RuntimeAttestation::Field::boolean
				(
					"target_executable", targetExecutable
				),
				RuntimeAttestation::Field::boolean
				(
					"trampoline_executable", trampolineExecutable
				),
				RuntimeAttestation::Field::boolean
				(
					"installed", this->size != 0 && targetExecutable
					             && trampolineExecutable
				),
			}
		);
	}

	g_pLog->debug
	(
		"Detour hooked %s (%p) with hook at %p and tramp at %p\n",
		this->name.c_str(),
		this->originalFn.address,
		this->hookFn.address,
		this->tramp.address
	);
}

template<typename T>
void DetourHook<T>::remove()
{
	if (!this->size)
	{
		return;
	}

	LM_UnhookCode(this->originalFn.address, this->tramp.address, this->size);
	this->size = 0;

	g_pLog->debug("Unhooked %s\n", this->name.c_str());
}

template<typename T>
void VFTHook<T>::place()
{
	LM_VmtHook(this->vft.get(), this->index, this->hookFn.address);
	this->hooked = true;

	g_pLog->debug
	(
		"VFT hooked %s (%p) with hook at %p\n",
		this->name.c_str(),
		this->originalFn.address,
		this->hookFn.address
	);
}

template<typename T>
void VFTHook<T>::remove()
{
	if (!this->hooked)
	{
		return;
	}

	LM_VmtUnhook(this->vft.get(), this->index);
	this->hooked = false;

	g_pLog->debug("Unhooked %s!\n", this->name.c_str());
}

template<typename T>
void VFTHook<T>::setup(std::shared_ptr<lm_vmt_t> vft, unsigned int index, T hookFn)
{
	this->vft = vft;
	this->index = index;

	this->originalFn.address = LM_VmtGetOriginal(this->vft.get(), this->index);
	this->hookFn.fn = hookFn;
}

__attribute__((hot))
static void hkTraceIPC(const char* iface, const char* fn)
{
	Hooks::TraceIPC.tramp.fn(iface, fn);

	if (g_config.extendedLogging.get())
	{
		g_pLog->debug
		(
			"%s(%s, %s)\n",

			Hooks::TraceIPC.name.c_str(),
			iface,
			fn
		);
	}
}

static uint32_t hkCAPIJob_GetPlayerStats(void* pAPIJob)
{
	uint32_t res = Hooks::CAPIJob_GetPlayerStats.tramp.fn(pAPIJob);

	g_pLog->debug
	(
		"%s(%p) -> %i\n",
		Hooks::CAPIJob_GetPlayerStats.name.c_str(),
		pAPIJob,
		res
	);

	return res;
}

static void hkProtoBufMsgBase_InitFromPacket(CProtoBufMsgBase* pMsg, void* pSrc)
{
	Hooks::CProtoBufMsgBase_InitFromPacket.tramp.fn(pMsg, pSrc);

	if (!pSrc)
	{
		return;
	}

	g_pLog->debug("Received ProtoBufMsg of type %u with type %s\n", pMsg->type, MemHlp::getTypeName(pMsg));

	Achievements::recvMessage(pMsg);
	DepotKey::recvMsg(pMsg);
	Misc::recvMsg(pMsg);
	PICS::recvMsg(pMsg);
	Ticket::recvMsg(pMsg);
}

static uint32_t hkProtoBufMsgBase_Send(CProtoBufMsgBase* pMsg)
{
	Apps::sendMsg(pMsg);
	DepotKey::sendMsg(pMsg);
	FakeAppIds::sendMsg(pMsg);

	const uint32_t ret = Hooks::CProtoBufMsgBase_Send.tramp.fn(pMsg);
	g_pLog->debug("Sending ProtoBufMsg of type %u with type %s\n", pMsg->type, MemHlp::getTypeName(pMsg));

	return ret;
}

static void hkSteamEngine_Init(void* pSteamEngine)
{
	Hooks::CSteamEngine_Init.tramp.fn(pSteamEngine);

	g_pSteamEngine = reinterpret_cast<CSteamEngine*>(pSteamEngine);
	g_pLog->debugOnce("g_pSteamEngine at %p\n", pSteamEngine);
}

static uint32_t hkSteamEngine_SetAppIdForCurrentPipe(void* pSteamEngine, uint32_t appId, bool a2)
{
	FakeAppIds::setAppIdForCurrentPipe(appId);

	const uint32_t ret = Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(pSteamEngine, appId, a2);

	g_pLog->debug
	(
		"%s(%p, %u, %i) -> %i\n",

		Hooks::CSteamEngine_SetAppIdForCurrentPipe.name.c_str(),
		pSteamEngine,
		appId,
		a2,
		ret
	);

	return ret;
}

static gameserverdetails_t* hkSteamMatchmakingServers_GetServerDetails(void* pSteamMatchmakingServers, uint32_t handle, uint32_t serverIdx)
{
	gameserverdetails_t* ret = Hooks::CSteamMatchmakingServers_GetServerDetails.tramp.fn(pSteamMatchmakingServers, handle, serverIdx);

	g_pLog->debug
	(
		"%s(%p, %p, %u) -> %p\n",

		Hooks::CSteamMatchmakingServers_GetServerDetails.name.c_str(),
		pSteamMatchmakingServers,
		handle,
		serverIdx,
		ret
	);

	if(ret)
	{
		FakeAppIds::getServerDetails(handle, *ret);
	}

	return ret;
}

static uint32_t hkSteamMatchmakingServers_RequestInternetServerList(void* pSteamMatchmakingServers, uint32_t appId, uint32_t a2, uint32_t a3, uint32_t a4)
{
	const uint32_t fake = FakeAppIds::requestInternetServerList(appId);

	uint32_t handle = Hooks::CSteamMatchmakingServers_RequestInternetServerList.tramp.fn(pSteamMatchmakingServers, fake ? fake : appId, a2, a3, a4);

	g_pLog->debug
	(
		"%s(%p, %u, %p, %p, %p)->%p\n",

		Hooks::CSteamMatchmakingServers_RequestInternetServerList.name.c_str(),
		pSteamMatchmakingServers,
		appId,
		a2,
		a3,
		a4,
		handle
	);

	FakeAppIds::fakeAppIdMapServer[handle] = appId;

	return handle;
}

__attribute__((hot))
static uint32_t hkUser_CheckAppOwnership(void* pClientUser, uint32_t appId, CAppOwnershipInfo* pOwnershipInfo)
{
	// Cache the local CUser* as a timing-independent fallback for
	// getLocalUser().  This hook fires early and constantly with the
	// pipe-0 user, so it covers the case where the one-shot
	// CSteamEngine::Init call happened before our hooks were placed
	// (LD_PRELOAD injection) and g_pSteamEngine is therefore null.
	if (pClientUser != nullptr)
	{
		g_pLocalUser = reinterpret_cast<CUser*>(pClientUser);
	}

	const auto statsContext = StatsPolicy::context();
	const uint32_t ret = Hooks::CUser_CheckAppOwnership.tramp.fn(pClientUser, appId, pOwnershipInfo);
	StatsPolicy::observe(statsContext, appId, ret != 0, pOwnershipInfo);

	g_pLog->debugOnce
	(
		"%s(%p, %u) -> %i\n",

		Hooks::CUser_CheckAppOwnership.name.c_str(),
		pClientUser,
		appId,
		ret
	);

	// Drive the one-shot package-0 license reconcile from here: this
	// hook fires early and repeatedly with a valid pipe-0 CUser, so it
	// is the reliable place to broadcast LicensesUpdated_t once the
	// AdditionalApps have been injected into package 0 — even on a cold
	// cache where the engine user map isn't populated when LoadPackage
	// runs.  No-op until injection has happened and after the single
	// broadcast.
	PackagePatch::tryReconcileLicenses();

	if (Apps::checkAppOwnership(appId, pOwnershipInfo) || DLC::checkAppOwnership(appId, pOwnershipInfo))
	{
		return true;
	}

	return ret;
}

static uint32_t hkUser_GetSubscribedApps(void* pClientUser, uint32_t* pAppList, uint32_t size, uint8_t a3)
{
	uint32_t count = Hooks::CUser_GetSubscribedApps.tramp.fn(pClientUser, pAppList, size, a3);

	Apps::getSubscribedApps(pAppList, size, count);

	g_pLog->debug
	(
		"%s(%p, %p, %i, %i) -> %i\n",

		Hooks::CUser_GetSubscribedApps.name.c_str(),
		pClientUser,
		pAppList,
		size,
		a3,
		count
	);

	return count;
}

static uint32_t hkUser_PostCallbackToAppId(void* pUser, uint32_t appId, uint32_t type, void* pCallback, uint32_t callbackSize)
{
	const uint32_t fakeAppId = FakeAppIds::getFakeAppId(appId);
	if (fakeAppId)
	{
		g_pLog->debug("Rerouting callback from %u to %u\n", appId, fakeAppId);
		appId = fakeAppId;
	}

	const uint32_t ret = Hooks::CUser_PostCallbackToAppId.tramp.fn(pUser, appId, type, pCallback, callbackSize);

	g_pLog->debug
	(
		"%s(%p, %u, %u, %p, %u) -> %u\n",

		Hooks::CUser_PostCallbackToAppId.name.c_str(),
		pUser,
		appId,
		type,
		pCallback,
		callbackSize,
		ret
	);

	return ret;
}

static bool hkClientAppManager_BCanRemotePlayTogether(void* pClientAppManager, uint32_t appId)
{
	const bool ret = Hooks::IClientAppManager_BCanRemotePlayTogether.tramp.fn(pClientAppManager, appId);
	g_pLog->debug
	(
		"%s(%p, %u) -> %u\n",
		Hooks::IClientAppManager_BCanRemotePlayTogether.name.c_str(),
		pClientAppManager,
		appId,
		ret
	);

	return true;
}

static void* hkClientAppManager_LaunchApp(void* pClientAppManager, uint32_t* pAppId, void* a2, void* a3, void* a4)
{
	if (pAppId)
	{
		g_pLog->debugOnce
		(
			"%s(%p, %u, %p, %p, %p)\n",

			Hooks::IClientAppManager_LaunchApp.name.c_str(),
			pClientAppManager,
			*pAppId,
			a2,
			a3,
			a4
		);

		FakeAppIds::launchApp(*pAppId);
		Ticket::launchApp(*pAppId);
		SteamStub::onLaunchApp(*pAppId);
	}

	return Hooks::IClientAppManager_LaunchApp.originalFn.fn(pClientAppManager, pAppId, a2, a3, a4);
}

static bool hkClientAppManager_IsAppDlcInstalled(void* pClientAppManager, uint32_t appId, uint32_t dlcId)
{
	const bool ret = Hooks::IClientAppManager_IsAppDlcInstalled.originalFn.fn(pClientAppManager, appId, dlcId);
	g_pLog->debugOnce
	(
		"%s(%p, %u, %u) -> %i\n",

		Hooks::IClientAppManager_IsAppDlcInstalled.name.c_str(),
		pClientAppManager,
		appId,
		dlcId,
		ret
	);

	if (DLC::isAppDlcInstalled(dlcId))
	{
		return true;
	}

	return ret;
}

static bool hkClientAppManager_BIsDlcEnabled(void* pClientAppManager, uint32_t appId, uint32_t dlcId, void* a3)
{
	const bool ret = Hooks::IClientAppManager_BIsDlcEnabled.originalFn.fn(pClientAppManager, appId, dlcId, a3);
	g_pLog->debugOnce
	(
		"%s(%p, %u, %u, %p) -> %i\n",

		Hooks::IClientAppManager_BIsDlcEnabled.name.c_str(),
		pClientAppManager,
		appId,
		dlcId,
		a3,
		ret
	);

	
	if (DLC::isDlcEnabled(appId, dlcId))
	{
		return true;
	}

	return ret;
}

static bool hkClientAppManager_GetUpdateInfo(void* pClientAppManager, uint32_t appId, uint32_t* a2)
{
	const bool success = Hooks::IClientAppManager_GetAppUpdateInfo.originalFn.fn(pClientAppManager, appId, a2);
	g_pLog->debugOnce("IClientAppManager::GetUpdateInfo(%p, %u, %p) -> %i\n", pClientAppManager, appId, a2, success);

	if (Apps::shouldDisableUpdates(appId))
	{
		g_pLog->infoOnce("Disabled updates for %u\n", appId);
		return false;
	}

	return success;
}

__attribute__((hot))
static void hkClientAppManager_RunIPCFrame(void* pClientAppManager, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientAppManager_RunIPCFrame.name);

	g_pClientAppManager = reinterpret_cast<IClientAppManager*>(pClientAppManager);

	std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
	LM_VmtNew(*reinterpret_cast<lm_address_t**>(pClientAppManager), vft.get());

	Hooks::IClientAppManager_BIsDlcEnabled.setup(vft, VFTIndexes::IClientAppManager::BIsDlcEnabled, hkClientAppManager_BIsDlcEnabled);
	Hooks::IClientAppManager_GetAppUpdateInfo.setup(vft, VFTIndexes::IClientAppManager::GetUpdateInfo, hkClientAppManager_GetUpdateInfo);
	Hooks::IClientAppManager_LaunchApp.setup(vft, VFTIndexes::IClientAppManager::LaunchApp, hkClientAppManager_LaunchApp);
	Hooks::IClientAppManager_IsAppDlcInstalled.setup(vft, VFTIndexes::IClientAppManager::IsAppDlcInstalled, hkClientAppManager_IsAppDlcInstalled);

	Hooks::IClientAppManager_BIsDlcEnabled.place();
	Hooks::IClientAppManager_GetAppUpdateInfo.place();
	Hooks::IClientAppManager_LaunchApp.place();
	Hooks::IClientAppManager_IsAppDlcInstalled.place();

	g_pLog->debug("IClientAppManager->vft at %p\n", vft->vtable);

	// Owner-frame accounting (see the note above hkClientUtils_RunIPCFrame):
	// every dispatcher that can run on the owner thread accounts its frame, so
	// ipc_depth/ipc_frames describe real owner activity. This one is a one-shot
	// (it unhooks itself below), but it still counts while it runs.
	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	Hooks::IClientAppManager_RunIPCFrame.remove();
	Hooks::IClientAppManager_RunIPCFrame.originalFn.fn(pClientAppManager, a1, a2, a3);
}

static int32_t hkClientApps_GetAppData(void* pClientApps, uint32_t appId, const char* name, char* pChOut, uint32_t outSize)
{
	const int32_t ret = Hooks::IClientApps_GetAppData.originalFn.fn(pClientApps, appId, name, pChOut, outSize);

	g_pLog->debugOnce
	(
		"%s(%u, %s, len=%u) -> %i\n",
		Hooks::IClientApps_GetAppData.name.c_str(),
		appId,
		name ? name : "(null)",
		outSize,
		ret
	);

	return ret;
}

static unsigned int hkClientApps_GetDLCCount(void* pClientApps, uint32_t appId)
{
	uint32_t count = Hooks::IClientApps_GetDLCCount.originalFn.fn(pClientApps, appId);
	g_pLog->debugOnce
	(
		"%s(%p, %u) -> %u\n",

		Hooks::IClientApps_GetDLCCount.name.c_str(),
		pClientApps,
		appId,
		count
	);
	
	appId = FakeAppIds::getRealAppIdForCurrentPipe();

	const uint32_t override = DLC::getDlcCount(appId);
	if (override)
	{
		return override;
	}

	return count;
}

static bool hkClientApps_GetDLCDataByIndex(void* pClientApps, uint32_t appId, int dlcIndex, uint32_t* pDlcId, bool* pIsAvailable, char* pChDlcName, size_t dlcNameLen)
{
	appId = FakeAppIds::getRealAppIdForCurrentPipe();

	bool ret = DLC::getDlcDataByIndex(
		appId, dlcIndex, pDlcId, pIsAvailable, pChDlcName, dlcNameLen);
	if (!ret)
	{
		ret = Hooks::IClientApps_GetDLCDataByIndex.originalFn.fn(
			pClientApps, appId, dlcIndex, pDlcId, pIsAvailable,
			pChDlcName, dlcNameLen);
		if (ret)
		{
			DLC::makeDlcAvailable(
				pDlcId ? *pDlcId : 0, pIsAvailable);
		}
	}


	g_pLog->debugOnce
	(
		"%s(%p, %u, %i, %p, %p, %s, %i) -> %i\n",

		Hooks::IClientApps_GetDLCDataByIndex.name.c_str(),
		pClientApps,
		appId,
		dlcIndex,
		pDlcId,
		pIsAvailable,
		pChDlcName,
		dlcNameLen,
		ret
	);

	return ret;
}

__attribute__((hot))
static void hkClientApps_RunIPCFrame(void* pClientApps, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientApps_RunIPCFrame.name);

	static bool hooked = false;
	if (!hooked)
	{
		g_pClientApps = reinterpret_cast<IClientApps*>(pClientApps);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(pClientApps), vft.get());

		Hooks::IClientApps_GetDLCDataByIndex.setup(vft, VFTIndexes::IClientApps::GetDLCDataByIndex, hkClientApps_GetDLCDataByIndex);
		Hooks::IClientApps_GetDLCCount.setup(vft, VFTIndexes::IClientApps::GetDLCCount, hkClientApps_GetDLCCount);
		Hooks::IClientApps_GetAppData.setup(vft, VFTIndexes::IClientApps::GetAppData, hkClientApps_GetAppData);

		Hooks::IClientApps_GetDLCDataByIndex.place();
		Hooks::IClientApps_GetDLCCount.place();
		Hooks::IClientApps_GetAppData.place();

		g_pLog->debug("IClientApps->vft at %p\n", vft->vtable);

		hooked = true;
	}

	// Extra drain opportunity, plus owner-frame accounting. Both are no-ops
	// unless this really is the latched owner thread. Measured on the guest:
	// adding these extra drain points did NOT shorten the owner wake cadence
	// (3.30 s -> 3.41 s), so they are not a latency mechanism — they are there
	// so whichever dispatcher runs first takes the work, and so the frame
	// counters cover all owner-thread IPC activity instead of one interface's.
	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	Hooks::IClientApps_RunIPCFrame.tramp.fn(pClientApps, a1, a2, a3);
}

static bool hkClientRemoteStorage_IsCloudEnabledForApp(void* pClientRemoteStorage, uint32_t appId)
{
	const bool enabled = Hooks::IClientRemoteStorage_IsCloudEnabledForApp.originalFn.fn(pClientRemoteStorage, appId);
	g_pLog->debugOnce
	(
		"%s(%p, %u) -> %i\n",

		Hooks::IClientRemoteStorage_IsCloudEnabledForApp.name.c_str(),
		pClientRemoteStorage,
		appId,
		enabled
	);

	if (Apps::shouldDisableCloud(appId))
	{
		g_pLog->infoOnce("Disabled cloud for %u\n", appId);
		return false;
	}

	return enabled;
}

static void hkClientRemoteStorage_RunIPCFrame(void* pClientRemoteStorage, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientRemoteStorage_RunIPCFrame.name);

	static bool hooked = false;
	if (!hooked)
	{
		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(pClientRemoteStorage), vft.get());

		Hooks::IClientRemoteStorage_IsCloudEnabledForApp.setup(vft, VFTIndexes::IClientRemoteStorage::IsCloudEnabledForApp, hkClientRemoteStorage_IsCloudEnabledForApp);
		Hooks::IClientRemoteStorage_IsCloudEnabledForApp.place();

		g_pLog->debug("IClientRemoteStorage->vft at %p\n", vft->vtable);

		hooked = true;
	}

	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	FakeAppIds::runIPCFrame(false);
	Hooks::IClientRemoteStorage_RunIPCFrame.tramp.fn(pClientRemoteStorage, a1, a2, a3);
	FakeAppIds::runIPCFrame(true);
}

static void hkClientUGC_RunIPCFrame(void* pClientUGC, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientUGC_RunIPCFrame.name);

	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	FakeAppIds::runIPCFrame(false);
	Hooks::IClientUGC_RunIPCFrame.tramp.fn(pClientUGC, a1, a2, a3);
	FakeAppIds::runIPCFrame(true);
}

static uint32_t hkClientUtils_GetAppId(void* pClientUtils)
{
	uint32_t appId = Hooks::IClientUtils_GetAppId.originalFn.fn(pClientUtils);

	g_pLog->debug
	(
		"%s(%p) -> %u\n",

		Hooks::IClientUtils_GetAppId.name.c_str(),
		pClientUtils,
		appId
	);

	const uint32_t real = FakeAppIds::getRealAppIdForCurrentPipe(false);
	if(real)
	{
		g_pLog->debug("Overwriting appId with %u\n", real);
		return real;
	}

	return appId;
}

static bool hkClientUtils_GetOfflineMode(void* pClientUtils)
{
	const bool ret = Hooks::IClientUtils_GetOfflineMode.originalFn.fn(pClientUtils);

	if (Misc::shouldFakeOffline())
	{
		return true;
	}

	return ret;
}

// The IClientUtils dispatcher defines the "owner IPC thread" for this process:
// it is the thread a controlled VM run latched and compared against, and it is
// where the owner TID is latched. Every hooked dispatcher then drains and
// accounts its frame, but only when it observes itself running on that latched
// thread. The drain sits inside the frame (before the original runs) so queued
// work is serialised with the dispatcher instead of racing it from an inotify
// pthread.
static void hkClientUtils_RunIPCFrame(void* pClientUtils, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientUtils_RunIPCFrame.name);

	static bool hooked = false;
	if (!hooked)
	{
		g_pClientUtils = reinterpret_cast<IClientUtils*>(pClientUtils);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(pClientUtils), vft.get());

		Hooks::IClientUtils_GetAppId.setup(vft, VFTIndexes::IClientUtils::GetAppId, hkClientUtils_GetAppId);
		Hooks::IClientUtils_GetOfflineMode.setup(vft, VFTIndexes::IClientUtils::GetOfflineMode, hkClientUtils_GetOfflineMode);

		Hooks::IClientUtils_GetAppId.place();
		Hooks::IClientUtils_GetOfflineMode.place();

		g_pLog->debug("IClientUtils->vft at %p\n", vft->vtable);

		hooked = true;
	}

	OwnerWork::latchOwnerThread();

	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	Hooks::IClientUtils_RunIPCFrame.tramp.fn(pClientUtils, a1, a2, a3);
}

static bool hkClientUser_BLoggedOn(void* pClientUser)
{
	const bool ret = Hooks::IClientUser_BLoggedOn.tramp.fn(pClientUser);
	
	if (Misc::shouldFakeOffline())
	{
		return false;
	}

	return ret;
}

static uint32_t hkClientUser_BUpdateOwnershipTicket(void* pClientUser, uint32_t appId, bool staleOnly)
{
	const auto cached = Ticket::getCachedTicket(appId);
	CUser* user = getLocalUser();
	if (user != nullptr && user->isSubscribed(appId) && !cached.steamId)
	{
		staleOnly = false;
		g_pLog->debug("Force re-requesting OwnershipInfo for %u\n", appId);
	}

	const uint32_t ret = Hooks::IClientUser_BUpdateAppOwnershipTicket.tramp.fn(pClientUser, appId, staleOnly);

	g_pLog->debug
	(
		"%s(%p, %u, %i) -> %u\n",

		Hooks::IClientUser_BUpdateAppOwnershipTicket.name.c_str(),
		pClientUser,
		appId,
		staleOnly,
		ret
	);

	return ret;
}

static std::vector<uint8_t> getClientLocalOwnershipTicket(void* pClientUser)
{
	// Query Steam's own per-user ticket store first. This keeps the common path
	// self-contained on fresh installs; the on-disk cache below is only needed
	// if the client store is temporarily unavailable (for example, offline).
	std::array<uint8_t, 1024> buffer {};
	uint32_t appIdOffset = 0;
	uint32_t steamIdOffset = 0;
	uint32_t signatureOffset = 0;
	uint32_t signatureSize = 0;
	const uint32_t size = Hooks::IClientUser_GetAppOwnershipTicketExtendedData.tramp.fn(
		pClientUser,
		AppTicket::kLocalSourceAppId,
		buffer.data(),
		static_cast<uint32_t>(buffer.size()),
		&appIdOffset,
		&steamIdOffset,
		&signatureOffset,
		&signatureSize);

	if (size == 0 || size > buffer.size())
	{
		return {};
	}

	const auto bytes = std::span<const uint8_t>(buffer.data(), size);
	if (!AppTicket::isLocalSourceTicket(bytes))
	{
		g_pLog->debugOnce(
			"AppTicket: client returned an unusable local source size=%u\n", size);
		return {};
	}

	return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

static uint32_t hkClientUser_GetAppOwnershipTicketExtendedData(
	void* pClientUser,
	uint32_t appId,
	void* pTicket,
	uint32_t ticketSize,
	uint32_t* a4,
	uint32_t* a5,
	uint32_t* a6,
	uint32_t* a7)

{
	const uint32_t ret = Hooks::IClientUser_GetAppOwnershipTicketExtendedData.tramp.fn(pClientUser, appId, pTicket, ticketSize, a4, a5, a6, a7);
	g_pLog->debugOnce("%s(%u)->%u\n", Hooks::IClientUser_GetAppOwnershipTicketExtendedData.name.c_str(), appId, ret);

	// Preserve a genuine client result.  Managed apps whose client lookup
	// returned no data may use either their cached signed ticket or a local
	// ticket derived from app 7.  This writes the direct IClientUser output
	// buffer, avoiding protobuf arena string mutation entirely.
	if (ret == 0 && g_config.isAddedAppId(appId))
	{
		const auto explicitTicket = Ticket::getCachedTicket(appId);
		auto localSource = getClientLocalOwnershipTicket(pClientUser);
		const bool localSourceFromClient = !localSource.empty();
		if (localSource.empty())
		{
			const auto cachedSource = Ticket::getCachedTicket(AppTicket::kLocalSourceAppId);
			localSource.assign(cachedSource.ticket.begin(), cachedSource.ticket.end());
		}
		const auto bytes = [](const std::string& value)
		{
			return std::span<const uint8_t>(
				reinterpret_cast<const uint8_t*>(value.data()), value.size());
		};

		const auto prepared = AppTicket::prepareOwnershipTicket(
			bytes(explicitTicket.ticket),
			std::span<const uint8_t>(localSource.data(), localSource.size()),
			appId);
		if (AppTicket::copyOwnershipTicket(
			prepared, pTicket, ticketSize, a4, a5, a6, a7))
		{
			const char* source = "explicit";
			if (prepared.source == AppTicket::Source::LocalDerived)
			{
				source = localSourceFromClient
					? "local-client-derived" : "local-cache-derived";
			}
			g_pLog->infoOnce(
				"AppTicket: served %s ownership ticket for app=%u "
				"physical=%zu logical=%u\n",
				source, appId, prepared.data.size(), prepared.totalSize);

			if (prepared.source == AppTicket::Source::Explicit)
			{
				Ticket::getTicketOwnershipExtendedData(appId);
			}
			return prepared.totalSize;
		}

		g_pLog->debugOnce(
			"AppTicket: no usable ownership ticket for app=%u capacity=%u\n",
			appId, ticketSize);
	}

	Ticket::getTicketOwnershipExtendedData(appId);

	return ret;
}

static uint8_t hkClientUser_IsUserSubscribedAppInTicket(void* pClientUser, uint32_t steamId, uint32_t a2, uint32_t a3, uint32_t appId)
{
	const uint8_t ticketState = Hooks::IClientUser_IsUserSubscribedAppInTicket.tramp.fn(pClientUser, steamId, a2, a3, appId);
	g_pLog->debug
	(
		"%s(%p, %u, %u, %u) -> %i\n",

		Hooks::IClientUser_IsUserSubscribedAppInTicket.name.c_str(),
		pClientUser,
		a2,
		a3,
		appId,
		ticketState
	);
	
	if (DLC::userSubscribedInTicket(appId))
	{
		return 0;
	}

	return ticketState;
}

__attribute__((stdcall))
static uint32_t hkClientUser_GetSteamId(uint32_t steamId)
{
	g_currentSteamId = steamId;
	StatsPolicy::setAccount(steamId);

	Ticket::SavedTicket ticket = Ticket::getCachedEncryptedTicket(FakeAppIds::getRealAppIdForCurrentPipe());

	if (ticket.steamId)
	{
		steamId = ticket.steamId;
	}
	else if (Ticket::oneTimeSteamIdSpoof)
	{
		steamId = Ticket::oneTimeSteamIdSpoof;
		Ticket::oneTimeSteamIdSpoof = 0;
	}

	return steamId;
}

static bool hkClientUser_RequiresLegacyCDKey(void* pClientUser, uint32_t appId, uint32_t* a2)
{
	const bool requiresKey = Hooks::IClientUser_RequiresLegacyCDKey.tramp.fn(pClientUser, appId, a2);
	g_pLog->debugOnce
	(
		"%s(%p, %u, %u) -> %i\n",

		Hooks::IClientUser_RequiresLegacyCDKey.name.c_str(),
		pClientUser,
		appId,
		a2,
		requiresKey
	);

	if (Apps::shouldDisableCDKey(appId))
	{
		g_pLog->infoOnce("Disable CD Key for %u\n", appId);
		// Zero the out-param before short-circuiting so the caller reads a
		// defined "no key" value (matches LumaCore's RequiresLegacyCDKey
		// suppression); leaving it untouched can let a stale value drive
		// the install-script %CDKEY% substitution.
		if (a2 != nullptr)
		{
			*a2 = 0;
		}
		return false;
	}

	return requiresKey;
}

static void hkClientUser_RunIPCFrame(void* pClientUser, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientUser_RunIPCFrame.name);

	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	Hooks::IClientUser_RunIPCFrame.tramp.fn(pClientUser, a1, a2, a3);
}

static void hkClientUserStats_RunIPCFrame(void* pClientUserStats, void* a1, void* a2, void* a3)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::IClientUserStats_RunIPCFrame.name);

	AffTrace::FrameGuard frame;
	OwnerWork::drainOnOwnerFrame();

	FakeAppIds::runIPCFrame(false);
	Hooks::IClientUserStats_RunIPCFrame.tramp.fn(pClientUserStats, a1, a2, a3);
	FakeAppIds::runIPCFrame(true);
}

static void hkSteamMatchmakingPingResponse_ServerResponded(void* pSteamMatchingPingResponse, gameserverdetails_t* details)
{
	FakeAppIds::pingResponse(details);
	Hooks::ISteamMatchmakingPingResponse_ServerResponded.tramp.fn(pSteamMatchingPingResponse, details);
}

static void patchRetn(lm_address_t address)
{
	// Defense-in-depth: never write to a null or unresolved address.
	// If a pattern fails to resolve, its address is 0 (or
	// LM_ADDRESS_BAD); patching it would segfault.  Skip instead.
	if (address == 0 || address == LM_ADDRESS_BAD)
	{
		g_pLog->warn("patchRetn called with invalid address %p; skipping\n", reinterpret_cast<void*>(address));
		return;
	}

	constexpr lm_byte_t retn = 0xC3;

	lm_prot_t oldProt;
	LM_ProtMemory(address, 1, LM_PROT_XRW, &oldProt); //LM_PROT_W Should be enough, but just in case something tries to execute it inbetween us setting the prot and writing to it
	LM_WriteMemory(address, &retn, 1);
	LM_ProtMemory(address, 1, oldProt, LM_NULL);
}

static lm_address_t hkNakedGetSteamId;
static bool createAndPlaceSteamIdHook()
{
	hkNakedGetSteamId = LM_AllocMemory(0, LM_PROT_XRW);
	if (hkNakedGetSteamId == LM_ADDRESS_BAD)
	{
		g_pLog->debug("Failed to allocate memory for GetSteamId!\n");
		return false;
	}

	g_pLog->debug("Allocated memory for GetSteamId hook at %p\n", hkNakedGetSteamId);

	auto insts = std::vector<lm_inst_t>();
	lm_address_t readAddr = Hooks::IClientUser_GetSteamId;
	for(;;)
	{
		lm_inst_t inst;
		if (!LM_Disassemble(readAddr, &inst))
		{
			g_pLog->debug("Failed to disassemble function at %p!\n", readAddr);
			return false;
		}

		insts.emplace_back(inst);
		readAddr = inst.address + inst.size;

		if (strcmp(inst.mnemonic, "ret") == 0)
		{
			break;
		}
	}

	const unsigned int retIdx = insts.size() - 1;

	g_pLog->debug("Ret is instruction number %u\n", retIdx);
	size_t totalBytes = 0;
	unsigned int instsToOverwrite = 0;
	for(int i = retIdx; i >= 0; i--)
	{
		lm_inst_t inst = insts.at(i);
		totalBytes += inst.size;
		instsToOverwrite++;

		if (totalBytes >= 5)
		{
			break;
		}
	}

	static uint32_t steamId;

	lm_address_t writeAddr = hkNakedGetSteamId;
	MemHlp::assembleCodeAt(writeAddr, "mov [%p], ecx", &steamId);
	MemHlp::assembleCodeAt(writeAddr, "pushad", nullptr);
	MemHlp::assembleCodeAt(writeAddr, "pushfd", nullptr);

	MemHlp::assembleCodeAt(writeAddr, "mov eax, %p", &hkClientUser_GetSteamId);
	MemHlp::assembleCodeAt(writeAddr, "mov ebx, [%p]", &steamId);
	MemHlp::assembleCodeAt(writeAddr, "push ebx", steamId);
	MemHlp::assembleCodeAt(writeAddr, "call eax", nullptr);
	MemHlp::assembleCodeAt(writeAddr, "mov [%p], eax", &steamId);

	MemHlp::assembleCodeAt(writeAddr, "popfd", nullptr);
	MemHlp::assembleCodeAt(writeAddr, "popad", nullptr);
	MemHlp::assembleCodeAt(writeAddr, "mov ecx, [%p]", &steamId);
	



	for (unsigned int i = 0; i < instsToOverwrite; i++)
	{
		lm_inst_t inst = insts.at(insts.size() - instsToOverwrite + i);
		memcpy(reinterpret_cast<void*>(writeAddr), inst.bytes, inst.size);

		writeAddr += inst.size;
		g_pLog->debug("Copied %s %s to tramp\n", inst.mnemonic, inst.op_str);
	}

	lm_address_t jmpAddr = insts.at(insts.size() - instsToOverwrite).address;
	g_pLog->debug("Placing jmp at %p\n", jmpAddr);

	lm_prot_t oldProt;
	LM_ProtMemory(jmpAddr, 5, LM_PROT_XRW, &oldProt);
	*reinterpret_cast<lm_byte_t*>(jmpAddr) = 0xE9;
	*reinterpret_cast<lm_address_t*>(jmpAddr + 1) = hkNakedGetSteamId - jmpAddr - 5;
	LM_ProtMemory(jmpAddr, 5, oldProt, nullptr);

	return true;
}

namespace Hooks
{
	DetourHook<TraceIPC_t> TraceIPC;

	DetourHook<IClientAppManager_RunIPCFrame_t> IClientAppManager_RunIPCFrame;
	DetourHook<IClientApps_RunIPCFrame_t> IClientApps_RunIPCFrame;
	DetourHook<IClientRemoteStorage_RunIPCFrame_t> IClientRemoteStorage_RunIPCFrame;
	DetourHook<IClientUGC_RunIPCFrame_t> IClientUGC_RunIPCFrame;
	DetourHook<IClientUtils_RunIPCFrame_t> IClientUtils_RunIPCFrame;
	DetourHook<IClientUser_RunIPCFrame_t> IClientUser_RunIPCFrame;
	DetourHook<IClientUserStats_RunIPCFrame_t> IClientUserStats_RunIPCFrame;

	DetourHook<CAPIJob_GetPlayerStats_t> CAPIJob_GetPlayerStats;

	DetourHook<CProtoBufMsgBase_InitFromPacket_t> CProtoBufMsgBase_InitFromPacket;
	DetourHook<CProtoBufMsgBase_Send_t> CProtoBufMsgBase_Send;

	DetourHook<CWebSocketConnection_BBuildAndAsyncSendFrame_t> CWebSocketConnection_BBuildAndAsyncSendFrame;
	DetourHook<CRemoteClientManager_RecvPkt_t> CRemoteClientManager_RecvPkt;
	DetourHook<CJobMgr_BRouteMsgToJob_t> CJobMgr_BRouteMsgToJob;
	DetourHook<CDepotDownloadMgr_BYldRequestDepotManifest_t> CDepotDownloadMgr_BYldRequestDepotManifest;

	DetourHook<CSteamMatchmakingServers_GetServerDetails_t> CSteamMatchmakingServers_GetServerDetails;
	DetourHook<CSteamMatchmakingServers_RequestInternetServerList_t> CSteamMatchmakingServers_RequestInternetServerList;

	DetourHook<CSteamEngine_Init_t> CSteamEngine_Init;
	DetourHook<CSteamEngine_SetAppIdForCurrentPipe_t> CSteamEngine_SetAppIdForCurrentPipe;

	DetourHook<CUser_CheckAppOwnership_t> CUser_CheckAppOwnership;
	DetourHook<CUser_GetSubscribedApps_t> CUser_GetSubscribedApps;
	DetourHook<CUser_PostCallbackToAppId_t> CUser_PostCallbackToAppId;

	DetourHook<IClientAppManager_BCanRemotePlayTogether_t> IClientAppManager_BCanRemotePlayTogether;

	DetourHook<IClientUser_BLoggedOn_t> IClientUser_BLoggedOn;
	DetourHook<IClientUser_BUpdateAppOwnershipTicket_t> IClientUser_BUpdateAppOwnershipTicket;
	DetourHook<IClientUser_GetAppOwnershipTicketExtendedData_t> IClientUser_GetAppOwnershipTicketExtendedData;
	DetourHook<IClientUser_IsUserSubscribedAppInTicket_t> IClientUser_IsUserSubscribedAppInTicket;
	DetourHook<IClientUser_RequiresLegacyCDKey_t> IClientUser_RequiresLegacyCDKey;

	VFTHook<IClientAppManager_BIsDlcEnabled_t> IClientAppManager_BIsDlcEnabled("IClientAppManager::BIsDlcEnabled");
	VFTHook<IClientAppManager_GetAppUpdateInfo_t> IClientAppManager_GetAppUpdateInfo("IClientAppManager::GetAppUpdateInfo");
	VFTHook<IClientAppManager_LaunchApp_t> IClientAppManager_LaunchApp("IClientAppManager::LaunchApp");
	VFTHook<IClientAppManager_IsAppDlcInstalled_t> IClientAppManager_IsAppDlcInstalled("IClientAppManager::IsAppDlcInstalled");

	VFTHook<IClientApps_GetDLCDataByIndex_t> IClientApps_GetDLCDataByIndex("IClientApps::GetDLCDataByIndex");
	VFTHook<IClientApps_GetDLCCount_t> IClientApps_GetDLCCount("IClientApps::GetDLCCount");
	VFTHook<IClientApps_GetAppData_t> IClientApps_GetAppData("IClientApps::GetAppData");

	VFTHook<IClientRemoteStorage_IsCloudEnabledForApp_t> IClientRemoteStorage_IsCloudEnabledForApp("IClientRemoteStorage::IsCloudEnabledForApp");

	VFTHook<IClientUtils_GetAppId_t> IClientUtils_GetAppId("IClientUtils::GetAppId");
	VFTHook<IClientUtils_GetOfflineMode_t> IClientUtils_GetOfflineMode("IClientUtils::GetOfflineMode");


	DetourHook<ISteamMatchmakingPingResponse_ServerResponded_t> ISteamMatchmakingPingResponse_ServerResponded;


	lm_address_t IClientUser_GetSteamId;
}

bool Hooks::setup()
{
	g_pLog->debug("Hooks::setup()\n");

	IClientUser_GetSteamId = Patterns::IClientUser::GetSteamId.address;

	bool succeeded =
		TraceIPC.setup(Patterns::TraceIPC, &hkTraceIPC)

		&& CAPIJob_GetPlayerStats.setup(Patterns::CAPIJob::GetPlayerStats, &hkCAPIJob_GetPlayerStats)

		&& CProtoBufMsgBase_InitFromPacket.setup(Patterns::CProtoBufMsgBase::InitFromPacket, &hkProtoBufMsgBase_InitFromPacket)
		&& CProtoBufMsgBase_Send.setup(Patterns::CProtoBufMsgBase::Send, &hkProtoBufMsgBase_Send)

		&& CWebSocketConnection_BBuildAndAsyncSendFrame.setup(Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame, &ManifestCode::hkBBuildAndAsyncSendFrame)
		&& CRemoteClientManager_RecvPkt.setup(Patterns::CRemoteClientManager::RecvPkt, &ManifestCode::hkRecvPkt)
		&& CJobMgr_BRouteMsgToJob.setup(Patterns::CJobMgr::BRouteMsgToJob, &ManifestCode::hkBRouteMsgToJob)
		&& CDepotDownloadMgr_BYldRequestDepotManifest.setup(Patterns::CDepotDownloadMgr::BYldRequestDepotManifest, &ManifestCode::hkCDepotDownloadMgr_BYldRequestDepotManifest)

		&& CSteamMatchmakingServers_GetServerDetails.setup(Patterns::CSteamMatchmakingServers::GetServerDetails, &hkSteamMatchmakingServers_GetServerDetails)
		&& CSteamMatchmakingServers_RequestInternetServerList.setup(Patterns::CSteamMatchmakingServers::RequestInternetServerList, &hkSteamMatchmakingServers_RequestInternetServerList)

		&& CUser_CheckAppOwnership.setup(Patterns::CUser::CheckAppOwnership, &hkUser_CheckAppOwnership)
		&& CUser_GetSubscribedApps.setup(Patterns::CUser::GetSubscribedApps, &hkUser_GetSubscribedApps)
		&& CUser_PostCallbackToAppId.setup(Patterns::CUser::PostCallbackToAppId, &hkUser_PostCallbackToAppId)

		&& CSteamEngine_Init.setup(Patterns::CSteamEngine::Init, &hkSteamEngine_Init)
		&& CSteamEngine_SetAppIdForCurrentPipe.setup(Patterns::CSteamEngine::SetAppIdForCurrentPipe, &hkSteamEngine_SetAppIdForCurrentPipe)

		&& IClientAppManager_BCanRemotePlayTogether.setup(Patterns::IClientAppManager::BCanRemotePlayTogether, hkClientAppManager_BCanRemotePlayTogether)

		&& IClientApps_RunIPCFrame.setup(Patterns::IClientApps::RunIPCFrame, hkClientApps_RunIPCFrame)
		&& IClientAppManager_RunIPCFrame.setup(Patterns::IClientAppManager::RunIPCFrame, hkClientAppManager_RunIPCFrame)
		&& IClientRemoteStorage_RunIPCFrame.setup(Patterns::IClientRemoteStorage::RunIPCFrame, hkClientRemoteStorage_RunIPCFrame)
		&& IClientUGC_RunIPCFrame.setup(Patterns::IClientUGC::RunIPCFrame, hkClientUGC_RunIPCFrame)
		&& IClientUtils_RunIPCFrame.setup(Patterns::IClientUtils::RunIPCFrame, hkClientUtils_RunIPCFrame)
		&& IClientUser_RunIPCFrame.setup(Patterns::IClientUser::RunIPCFrame, hkClientUser_RunIPCFrame)
		&& IClientUserStats_RunIPCFrame.setup(Patterns::IClientUserStats::RunIPCFrame, hkClientUserStats_RunIPCFrame)

		&& IClientUser_BLoggedOn.setup(Patterns::IClientUser::BLoggedOn, &hkClientUser_BLoggedOn)
		&& IClientUser_BUpdateAppOwnershipTicket.setup(Patterns::IClientUser::BUpdateAppOwnershipTicket, hkClientUser_BUpdateOwnershipTicket)
		&& IClientUser_GetAppOwnershipTicketExtendedData.setup(Patterns::IClientUser::GetAppOwnershipTicketExtendedData, hkClientUser_GetAppOwnershipTicketExtendedData)
		&& IClientUser_IsUserSubscribedAppInTicket.setup(Patterns::IClientUser::IsUserSubscribedAppInTicket, &hkClientUser_IsUserSubscribedAppInTicket)
		&& IClientUser_RequiresLegacyCDKey.setup(Patterns::IClientUser::RequiresLegacyCDKey, hkClientUser_RequiresLegacyCDKey)

		&& ISteamMatchmakingPingResponse_ServerResponded.setup(Patterns::ISteamMatchmakingPingResponse::ServerResponded, hkSteamMatchmakingPingResponse_ServerResponded);

	Hooks::place();

	// AppInfoState keeps this non-owning Store* in its hot path.  The static
	// lifetime is part of the manual-detour contract: it must outlive every
	// installed or disabled-hooked detour and every in-flight reader.  Task 7's
	// coordinator Store must provide the same lifetime before rebinding here.
	static HotReloadState::Store bootstrapStore;
	(void)AppInfoState::setup(bootstrapStore);

	PackagePatch::setup();
	(void)LibraryRemoval::setup();
	ManifestBind::setup();
	DepotQuarantine::setup();
	ReconcilePin::setup();
	Parental::setup();

	return succeeded;
}

void Hooks::place()
{
	// Mark the placement pass as ENTERED before the first hook goes in.
	//
	// Hooks::remove() is NOT only the teardown path: main.cpp's load() runs
	// once per audited module open, and its "the other module isn't mapped yet"
	// retry routes through unload() -> Hooks::remove() before anything is
	// hooked (LM_FindModule("steamui.so") fails on the first steamclient.so
	// la_objopen). That was a harmless no-op before the owner-thread work queue
	// existed. Closing the queue from there refuses every watcher-originated
	// Steam call for the WHOLE SESSION — the work is abandoned, not merely
	// executed elsewhere: no package-0 injection and no license broadcast for
	// as long as the client runs.
	//
	// Marking here (rather than at the end of place()) means a teardown that
	// interrupts a partial placement still counts as a real teardown, which is
	// the safe direction: closing the queue when hooks may be live is correct,
	// leaving it open when none are is correct too.
	OwnerWork::notePlacement();

	if (g_config.disableFamilyLock.get())
	{
		patchRetn(Patterns::FamilyGroupRunningApp.address);
		patchRetn(Patterns::StopPlayingBorrowedApp.address);
	}

	TraceIPC.place();

	CAPIJob_GetPlayerStats.place();

	CProtoBufMsgBase_InitFromPacket.place();
	CProtoBufMsgBase_Send.place();

	CWebSocketConnection_BBuildAndAsyncSendFrame.place();
	CRemoteClientManager_RecvPkt.place();
	CJobMgr_BRouteMsgToJob.place();
	CDepotDownloadMgr_BYldRequestDepotManifest.place();

	CSteamEngine_Init.place();
	CSteamEngine_SetAppIdForCurrentPipe.place();

	CSteamMatchmakingServers_GetServerDetails.place();
	CSteamMatchmakingServers_RequestInternetServerList.place();

	CUser_CheckAppOwnership.place();
	CUser_GetSubscribedApps.place();
	CUser_PostCallbackToAppId.place();

	IClientAppManager_BCanRemotePlayTogether.place();

	IClientApps_RunIPCFrame.place();
	IClientAppManager_RunIPCFrame.place();
	IClientRemoteStorage_RunIPCFrame.place();
	IClientUGC_RunIPCFrame.place();
	IClientUtils_RunIPCFrame.place();
	IClientUser_RunIPCFrame.place();
	IClientUserStats_RunIPCFrame.place();

	IClientUser_BLoggedOn.place();
	IClientUser_BUpdateAppOwnershipTicket.place();
	IClientUser_GetAppOwnershipTicketExtendedData.place();
	IClientUser_IsUserSubscribedAppInTicket.place();
	IClientUser_RequiresLegacyCDKey.place();

	ISteamMatchmakingPingResponse_ServerResponded.place();

	createAndPlaceSteamIdHook();
}

void Hooks::remove()
{
	// Close the owner-thread work queue FIRST: after this nothing new is
	// accepted and anything pending is abandoned, so unhooking can never race
	// a fresh watcher-originated Steam-owned call in.
	//
	// Only on a real teardown, though — see the placement note in
	// Hooks::place(). shutdownIfPlaced() is idempotent, so a repeated teardown
	// is a no-op and the benign pre-hook cleanup path leaves the queue
	// accepting work.
	if (OwnerWork::shutdownIfPlaced())
	{
		g_pLog->info("Hooks::remove: owner-thread work queue closed\n");
	}
	else
	{
		g_pLog->info("Hooks::remove: no hook placement to tear down; owner-thread work "
		             "queue left accepting work\n");
	}

	// Stop the optional UI queue before any of its SteamUI detours are restored.
	LibraryRemoval::remove();

	// AppInfoState performs the quiescent manual five-byte restore here, before
	// any other hook or package state can tear down the original call path.
	AppInfoState::remove();

	TraceIPC.remove();

	CAPIJob_GetPlayerStats.remove();

	CProtoBufMsgBase_InitFromPacket.remove();
	CProtoBufMsgBase_Send.remove();

	CWebSocketConnection_BBuildAndAsyncSendFrame.remove();
	CRemoteClientManager_RecvPkt.remove();
	CJobMgr_BRouteMsgToJob.remove();
	CDepotDownloadMgr_BYldRequestDepotManifest.remove();

	CSteamEngine_Init.remove();
	CSteamEngine_SetAppIdForCurrentPipe.remove();

	CSteamMatchmakingServers_GetServerDetails.remove();
	CSteamMatchmakingServers_RequestInternetServerList.remove();

	CUser_CheckAppOwnership.remove();
	CUser_GetSubscribedApps.remove();
	CUser_PostCallbackToAppId.remove();

	IClientAppManager_BCanRemotePlayTogether.remove();

	IClientApps_RunIPCFrame.remove();
	IClientAppManager_RunIPCFrame.remove();
	IClientRemoteStorage_RunIPCFrame.remove();
	IClientUGC_RunIPCFrame.remove();
	IClientUtils_RunIPCFrame.remove();
	IClientUser_RunIPCFrame.remove();
	IClientUserStats_RunIPCFrame.remove();

	IClientUser_BLoggedOn.remove();
	IClientUser_BUpdateAppOwnershipTicket.remove();
	IClientUser_GetAppOwnershipTicketExtendedData.remove();
	IClientUser_IsUserSubscribedAppInTicket.remove();
	IClientUser_RequiresLegacyCDKey.remove();

	ISteamMatchmakingPingResponse_ServerResponded.remove();

	IClientAppManager_BIsDlcEnabled.remove();
	IClientAppManager_GetAppUpdateInfo.remove();
	IClientAppManager_LaunchApp.remove();
	IClientAppManager_IsAppDlcInstalled.remove();

	IClientApps_GetDLCDataByIndex.remove();
	IClientApps_GetDLCCount.remove();

	IClientRemoteStorage_IsCloudEnabledForApp.remove();

	IClientUtils_GetAppId.remove();
	
	PackagePatch::remove();
	ManifestBind::remove();
	DepotQuarantine::remove();
	ReconcilePin::remove();
	Parental::remove();

	if (hkNakedGetSteamId != LM_ADDRESS_BAD)
	{
		LM_FreeMemory(hkNakedGetSteamId, 0);
	}
}
