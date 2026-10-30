#include "hooks.hpp"
#include "api.hpp"

#include "config.hpp"
#include "decompiler.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "lua.hpp"
#include "memhlp.hpp"
#include "patterns.hpp"
#include "vftableinfo.hpp"

#include "sdk/CNetPacket.hpp"
#include "sdk/CProtoBufMsgBase.hpp"
#include "sdk/CSteamEngine.hpp"
#include "sdk/CSteamMatchmakingServers.hpp"
#include "sdk/CUtl.hpp"
#include "sdk/CUser.hpp"
#include "sdk/types.hpp"
#include "sdk/IClientAppManager.hpp"
#include "sdk/IClientApps.hpp"
#include "sdk/IClientCompat.hpp"
#include "sdk/IClientFriends.hpp"
#include "sdk/IClientUser.hpp"
#include "sdk/IClientUtils.hpp"

#include "feats/achievements.hpp"
#include "feats/stats_policy.hpp"
#include "feats/appinfostate.hpp"
#include "feats/appticket.hpp"
#include "feats/apps.hpp"
#include "feats/cmrecv.hpp"
#include "feats/depotkey.hpp"
#include "feats/manifeststore.hpp"
#include "utils/ManifestFetch.hpp"
#include "feats/depotquarantine.hpp"
#include "feats/dlc.hpp"
#include "feats/manifestcode.hpp"
#include "feats/manifestdonor.hpp"
#include "feats/manifestbind.hpp"
#include "feats/misc.hpp"
#include "feats/fakeappid.hpp"
#include "feats/familyshare.hpp"
#include "feats/libraryremoval.hpp"
#include "feats/packagepatch.hpp"
#include "feats/parental.hpp"
#include "feats/pics.hpp"
#include "feats/reconcilepin.hpp"
#include "feats/steamstub.hpp"
#include "feats/ticket.hpp"
#include "ownerwork.hpp"
#include "runtime_attestation.hpp"

#include "libmem/libmem.h"

#include <array>
#include <atomic>
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
#include <utility>
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
VFTHook<T>::VFTHook() : VFTHook<T>("")
{
}

template<typename T>
bool DetourHook<T>::setup(const char* name, const lm_address_t fn, T hookFn)
{
	if (!isExecutableAddress(fn))
	{
		return false;
	}

	this->name = name;
	this->originalFn.address = fn;
	this->hookFn.fn = hookFn;

	return true;
}

template<typename T>
bool DetourHook<T>::setup(const Pattern_t& pattern, T hookFn)
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
bool DetourHook<T>::setup(const VFTableInfo_t& info, T hookFn)
{
	this->name = info.getPrintName();
	this->originalFn.address = info.address;
	this->hookFn.fn = hookFn;

	// A failed optional registry entry keeps address 0 (VFTableInfo_t::init).
	return info.address != 0;
}

template<typename T>
bool DetourHook<T>::place()
{
	this->size = LM_HookCode(this->originalFn.address, this->hookFn.address, &this->tramp.address);
	if (this->size)
		MemHlp::fixPICThunkCall(this->name.c_str(), this->originalFn.address, this->tramp.address);
	else
		g_pLog->warn("Unable to place the %s hook\n", this->name.c_str());

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

	if (!this->size)
		return false;
	g_pLog->debug
	(
		"Detour hooked %s (%p) with hook at 0x%lx and tramp at 0x%lx\n",
		this->name.c_str(),
		reinterpret_cast<void*>(this->originalFn.address),
		this->hookFn.address,
		this->tramp.address
	);
	return true;
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

	LOG_DEBUG("Unhooked %s\n", this->name.c_str());
}

template<typename T>
bool VFTHook<T>::place()
{
	this->hooked = LM_VmtHook(this->vft.get(), this->index, this->hookFn.address);
	if (!this->hooked)
	{
		g_pLog->warn("Unable to place the %s hook\n", this->name.c_str());
		return false;
	}

	LOG_DEBUG
	(
		"VFT hooked %s (0x%lx) with hook at 0x%lx\n",
		this->name.c_str(),
		this->originalFn.address,
		this->hookFn.address
	);
	return true;
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

	LOG_DEBUG("Unhooked %s!\n", this->name.c_str());
}

template<typename T>
void VFTHook<T>::setup(std::shared_ptr<lm_vmt_t> vft, const VFTableInfo_t& info, T hookFn)
{
	this->vft = vft;
	this->index = info.index;
	this->name = info.getPrintName();

	this->originalFn.address = LM_VmtGetOriginal(this->vft.get(), this->index);
	this->hookFn.fn = hookFn;
}

__attribute__((hot))
static void hkTraceIPC(const char* iface, const char* fn)
{
	if (g_config.extendedLogging.get())
	{
		LOG_DEBUG
		(
			"%s(%s, %s)\n",

			Hooks::TraceIPC.name.c_str(),
			iface,
			fn
		);
	}

	LOG_TRACE("Calling tramp\n");
	Hooks::TraceIPC.tramp.fn(iface, fn);
}

std::unordered_map<int, LuaHook*> LuaHook::hooks;

LuaHook::LuaHook(const char* name, void* targetFn)
	:
	name(name),
	fn(reinterpret_cast<lm_address_t>(targetFn))
{
	for (int i = 0; ; i++)
	{
		if (!hooks.contains(i))
		{
			idx = i;
			break;
		}
	}

	hooks[idx] = this;
	LOG_DEBUG("Created LuaHook %s (%i) from %p\n", name, idx, targetFn);
}

LuaHook::~LuaHook()
{
	remove();
	hooks.erase(idx);
}

void* LuaHook::place()
{
	if (fn == LM_ADDRESS_BAD || hookFn == LM_ADDRESS_BAD)
	{
		LOG_ERROR("Failed to place LuaHook %s, targetFn is LM_ADDRESS_BAD\n", name.c_str());
		return nullptr;
	}

	size = LM_HookCode(fn, hookFn, &tramp);
	if (!size)
	{
		return nullptr;
	}

	MemHlp::fixPICThunkCall(name.c_str(), fn, tramp);
	LOG_DEBUG("Placed LuaHook %s from %p to %p\n", name.c_str(),
	          reinterpret_cast<void*>(fn), reinterpret_cast<void*>(hookFn));
	return reinterpret_cast<void*>(tramp);
}

bool LuaHook::remove()
{
	if (!size)
	{
		return false;
	}

	LM_UnhookCode(fn, tramp, size);
	size = 0;
	LOG_DEBUG("Removed LuaHook %s\n", name.c_str());
	return true;
}

static void hkCMInterface_RecvPkt(void* pCMInterface, CNetPacket* pNetPacket)
{
	// Settle the packet field layout from the first live Steam-owned packet
	// before any field is read.  No-op once settled; stays at shift 0 (the
	// stable layout) unless a packet fails to validate there.
	CNetPacket::detectLayout(pNetPacket);

	if (g_config.extendedLogging.get())
	{
		g_pLog->debug(
			"RecvPkt %u\n",
			pNetPacket ? pNetPacket->getType() : CNetPacket::INVALID_MESSAGE_TYPE);
	}

	if (pNetPacket && pNetPacket->isProtoBuf())
	{
		if (FamilyShare::shouldChoke(g_config.disableFamilyLock.get(), *pNetPacket))
		{
			g_pLog->debug("Choking Family Share message %u\n",
			              static_cast<uint32_t>(pNetPacket->getProtoBufType()));
			pNetPacket->free();
			return;
		}

		// Route the CM protobuf frame through the message features (depot key,
		// PICS, ticket, wallet/email, license list, login-state).
		// dispatchNetPacket may rewrite the packet body in place before Steam
		// processes it.
		Lua::fireCallback(Lua::Callbacks::Network_RecvPkt, pNetPacket);
		CmRecv::dispatchNetPacket(pNetPacket);
	}

	LOG_TRACE("Calling tramp\n");
	Hooks::CCMInterface_RecvPkt.tramp.fn(pCMInterface, pNetPacket);
}

__attribute__((hot))
static uint32_t hkSteamEngine_ProcessIPCFrame(
	void* pSteamEngine,
	HSteamPipe hPipe,
	CUtlBuffer* pBufIn,
	CUtlBuffer* pBufOut)
{
	static std::once_flag attestationOnce;
	attestHookInvocation(attestationOnce, Hooks::CSteamEngine_ProcessIPCFrame.name);

	if (!g_pSteamEngine)
	{
		g_pSteamEngine = reinterpret_cast<CSteamEngine*>(pSteamEngine);
	}

	if (!pBufIn || !pBufIn->hasBytes(sizeof(EIPCCmd)))
	{
		return Hooks::CSteamEngine_ProcessIPCFrame.tramp.fn(
			pSteamEngine, hPipe, pBufIn, pBufOut);
	}

	EIPCCmd cmd{};
	std::memcpy(&cmd, pBufIn->mem.base, sizeof(cmd));
	const bool log = g_config.extendedLogging.get();
	if (log)
	{
		g_pLog->debug(
			"ProcessIPCFrame pipe %u command %s\n",
			hPipe,
			EIPCCmd_ToString(cmd).c_str());
	}

	uint32_t ret;
	if (cmd == EIPCCmd::RunInterface && pBufIn->hasBytes(10))
	{
		Hooks::placeVFTHooks();

		EIPCInterface interface{};
		uint32_t function = 0;
		std::memcpy(&interface, pBufIn->mem.base + 1, sizeof(interface));
		std::memcpy(&function, pBufIn->mem.base + 6, sizeof(function));
		// IClientUtils calls define the owner IPC thread: owner work queued
		// by watcher threads only runs inside a frame on that thread, so it
		// is serialised with Steam's dispatcher.
		if (interface == EIPCInterface::Utils)
		{
			OwnerWork::latchOwnerThread();
		}
		if (log)
		{
			const auto utils = g_pSteamEngine->getUtils();
			g_pLog->debug(
				"RunInterface %s %u for %u (%u)\n",
				EIPCInterface_ToString(interface).c_str(),
				function,
				FakeAppIds::getRealAppIdForCurrentPipe(),
				utils ? utils->getAppId() : 0);
		}
		FakeAppIds::runIPCFrame(false, interface);
		OwnerWork::drainOnOwnerFrame();
		ret = Hooks::CSteamEngine_ProcessIPCFrame.tramp.fn(
			pSteamEngine, hPipe, pBufIn, pBufOut);
		FakeAppIds::runIPCFrame(true, interface);

		if (interface == EIPCInterface::Utils)
		{
			SLSAPI::runIPCFrame();
		}
	}
	else
	{
		ret = Hooks::CSteamEngine_ProcessIPCFrame.tramp.fn(
			pSteamEngine, hPipe, pBufIn, pBufOut);
	}

	if (cmd == EIPCCmd::ClosePipe)
	{
		FakeAppIds::closePipe(hPipe);
	}

	return ret;
}

static uint32_t hkSteamEngine_SetAppIdForCurrentPipe(void* pSteamEngine, uint32_t appId, bool a2)
{
	FakeAppIds::setAppIdForCurrentPipe(appId);

	const uint32_t ret = Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(pSteamEngine, appId, a2);

	LOG_DEBUG
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

static gameserverdetails_t* hkSteamMatchmakingServers_GetServerDetails(void* pSteamMatchmakingServers, HServerRequest handle, uint32_t serverIdx)
{
	LOG_TRACE("Calling tramp\n");
	gameserverdetails_t* ret = Hooks::CSteamMatchmakingServers_GetServerDetails.tramp.fn(pSteamMatchmakingServers, handle, serverIdx);

	LOG_DEBUG
	(
		"%s(%p, %p, %u) -> %p\n",
		Hooks::CSteamMatchmakingServers_GetServerDetails.name.c_str(),
		pSteamMatchmakingServers,
		handle,
		serverIdx,
		reinterpret_cast<void*>(ret)
	);

	if (ret)
	{
		FakeAppIds::getServerDetails(handle, *ret);
	}

	return ret;
}

static HServerRequest hkSteamMatchmakingServers_RequestInternetServerList(void* pSteamMatchmakingServers, AppId_t appId, void* a2, uint32_t a3, void* a4)
{
	const uint32_t fake = FakeAppIds::requestInternetServerList(appId);
	LOG_TRACE("Calling tramp\n");
	HServerRequest handle = Hooks::CSteamMatchmakingServers_RequestInternetServerList.tramp.fn(pSteamMatchmakingServers, fake ? fake : appId, a2, a3, a4);
	LOG_DEBUG
	(
		"%s(%p, %u, %p, %u, %p) -> %p\n",

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
static bool hkUser_CheckAppOwnership(void* pClientUser, uint32_t appId, AppOwnershipInfo_t* pOwnershipInfo)
{
	// Cache the local CUser* as a timing-independent fallback for
	// getLocalUser().  This hook fires early and constantly with the
	// pipe-0 user, so it covers the time before the first
	// ProcessIPCFrame captures g_pSteamEngine.
	if (pClientUser != nullptr)
	{
		g_pLocalUser = reinterpret_cast<CUser*>(pClientUser);
	}

	// Only added apps are tracked; the other games' checks skip the policy lock.
	const bool tracked = g_config.isAddedAppId(appId);
	const auto statsContext = tracked ? StatsPolicy::context() : StatsPolicy::Context{};
	const bool ret = Hooks::CUser_CheckAppOwnership.tramp.fn(pClientUser, appId, pOwnershipInfo);
	if (tracked) StatsPolicy::observe(statsContext, appId, ret, pOwnershipInfo);

	g_pLog->debugOnce
	(
		"%s(%p, %u) -> %i\n",

		Hooks::CUser_CheckAppOwnership.name.c_str(),
		pClientUser,
		appId,
		ret
	);

	// Fires early and often with a valid pipe-0 CUser: retries the pending license
	// reconcile (pre-snapshot boot path, then once per unprocessed boot generation).
	PackagePatch::tryReconcileLicenses();

	if (Apps::checkAppOwnership(appId, pOwnershipInfo) || DLC::checkAppOwnership(appId, pOwnershipInfo))
	{
		return true;
	}

	return ret;
}

static uint32_t hkUser_GetSubscribedApps(void* pClientUser, uint32_t* pAppList, uint32_t size, uint8_t a3)
{
	LOG_TRACE("Calling tramp\n");
	uint32_t count = Hooks::CUser_GetSubscribedApps.tramp.fn(pClientUser, pAppList, size, a3);

	Apps::getSubscribedApps(pAppList, size, count);

	LOG_DEBUG
	(
		"%s(%p, %p, %i, %i) -> %i\n",

		Hooks::CUser_GetSubscribedApps.name.c_str(),
		pClientUser,
		reinterpret_cast<void*>(pAppList),
		size,
		a3,
		count
	);

	return count;
}

static uint32_t hkUser_SpawnGameId
(
	void* pUser,
	const char* exe,
	const char* cmd,
	const char* workingDir,
	GameId_t* pGameId,
	const char* title,
	int32_t a6,
	int32_t a7,
	int32_t a8,
	int32_t a9,
	void* a10
)
{
	std::string newExe = exe ? exe : "";
	std::string newCmd = cmd ? cmd : "";

	Apps::spawnGame(pGameId, newExe, newCmd);
	// The 64-bit client's games are spawned by srt-client-container, whose
	// environment still carries LD_AUDIT; drop it for the game (upstream
	// b464e69). A user-supplied LD_AUDIT in the launch options is kept.
	if (cmd && newCmd.find("LD_AUDIT=") == std::string::npos)
	{
		newExe = "env";
		newCmd = "env -u LD_AUDIT " + newCmd;
	}
	g_pLog->debug("Launching gameId %llu with cmd %s as %s\n",
	              static_cast<unsigned long long>(pGameId ? *pGameId : 0), newExe.c_str(), newCmd.c_str());
	return Hooks::CUser_SpawnGameId.tramp.fn
	(
		pUser,
		exe || cmd ? newExe.c_str() : nullptr,
		cmd ? newCmd.c_str() : nullptr,
		workingDir,
		pGameId,
		title,
		a6,
		a7,
		a8,
		a9,
		a10
	);
}

static bool hkUserAppManager_BuildDepotDependency
(
	void* pClientAppManager,
	AppId_t appId,
	void* a2,
	CUtlVector<DepotInfo_t>* depots,
	CUtlVector<DepotInfo_t>* sharedDepots,
	void* a5,
	uint32_t* pBuildId,
	bool* a7
)
{
	const bool success = Hooks::CUserAppManager_BuildDepotDependency.tramp.fn(pClientAppManager, appId, a2, depots, sharedDepots, a5, pBuildId, a7);
	g_pLog->debug("%s(%p, %u) -> %i\n", Hooks::CUserAppManager_BuildDepotDependency.name.c_str(), pClientAppManager, appId, success);

	Apps::buildDepotDependency(appId, depots, sharedDepots);

	// Steam checks depotcache for each planned manifest about 15 s later and
	// only requests a manifest code (Access Denied for added apps) when it is
	// missing. ManifestBind's CDepotDownloadMgr::BuildDepotDependency hook
	// starts these fetches after applying manifest pins; when that hook is not
	// placed, start them here for the public gids. The fetch is deduplicated
	// and runs on SLSsteam's executor; this hook does not wait for it.
	for (uint32_t i = 0; depots && !ManifestBind::builderInstalled() && i < depots->size; i++)
	{
		const auto* depot = depots->at(i);
		if (depot->manifestId
		    && DepotKey::manifestInManagedScope(depot->appId, depot->depotId)
		    && !ManifestStore::isInDepotcache(depot->depotId, depot->manifestId))
		{
			ManifestFetch::submitManifestBlob(depot->manifestId, depot->appId, depot->depotId,
				ManifestFetch::isAnyManagedDownloadActive(depot->appId));
		}
	}

	return success;
}

static uint32_t hkUser_PostCallbackToAppId(void* pUser, uint32_t appId, uint32_t type, void* pCallback, uint32_t callbackSize)
{
	const uint32_t fakeAppId = FakeAppIds::getFakeAppId(appId);
	if (fakeAppId)
	{
		LOG_DEBUG("Rerouting callback from %u to %u\n", appId, fakeAppId);
		appId = fakeAppId;
	}

	LOG_TRACE("Calling tramp\n");
	const uint32_t ret = Hooks::CUser_PostCallbackToAppId.tramp.fn(pUser, appId, type, pCallback, callbackSize);

	LOG_DEBUG
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
	LOG_TRACE("Calling original\n");
	const bool ret = Hooks::IClientAppManager_BCanRemotePlayTogether.originalFn.fn(pClientAppManager, appId);

	LOG_DEBUG
	(
		"%s(%p, %u) -> %u\n",
		Hooks::IClientAppManager_BCanRemotePlayTogether.name.c_str(),
		pClientAppManager,
		appId,
		ret
	);

	return true;
}

static void* hkClientAppManager_LaunchApp(void* pClientAppManager, GameId_t* pGameId, uint32_t a2, uint32_t a3, const char* a4)
{
	if (pGameId)
	{
		const auto appId = static_cast<uint32_t>(*pGameId);
		g_pLog->debugOnce
		(
			"%s(%p, %llu, %u, %u, %p)\n",
			Hooks::IClientAppManager_LaunchApp.name.c_str(),
			pClientAppManager,
			static_cast<unsigned long long>(*pGameId),
			a2,
			a3,
			static_cast<const void*>(a4)
		);
		FakeAppIds::launchApp(*pGameId);
		Ticket::launchApp(appId);
		SteamStub::onLaunchApp(appId);
	}
	return Hooks::IClientAppManager_LaunchApp.originalFn.fn(pClientAppManager, pGameId, a2, a3, a4);
}

static bool hkClientAppManager_IsAppDlcInstalled(void* pClientAppManager, uint32_t appId, uint32_t dlcId)
{
	LOG_TRACE("Calling original\n");
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
	LOG_TRACE("Calling original\n");

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
	LOG_TRACE("Calling original\n");

	const bool success = Hooks::IClientAppManager_GetAppUpdateInfo.originalFn.fn(pClientAppManager, appId, a2);
	g_pLog->debugOnce("IClientAppManager::GetUpdateInfo(%p, %u, %p) -> %i\n", pClientAppManager, appId, static_cast<void*>(a2), success);

	if (Apps::shouldDisableUpdates(appId))
	{
		g_pLog->debugOnce("Disabled updates for %u\n", appId);
		return false;
	}

	return success;
}

static bool hkClientAppManager_GetAppStateInfo(void* pClientAppManager, uint32_t appId, AppStateInfo_t* pInfo)
{
	const bool success = Hooks::IClientAppManager_GetAppStateInfo.originalFn.fn(pClientAppManager, appId, pInfo);

	if (success && pInfo)
	{
		Apps::getAppStateInfo(appId, pInfo);
	}

	return success;
}

static unsigned int hkClientApps_GetDLCCount(void* pClientApps, uint32_t appId)
{
	LOG_TRACE("Calling original\n");

	uint32_t count = Hooks::IClientApps_GetDLCCount.originalFn.fn(pClientApps, appId);
	g_pLog->debugOnce
	(
		"%s(%p, %u) -> %u\n",

		Hooks::IClientApps_GetDLCCount.name.c_str(),
		pClientApps,
		appId,
		count
	);
	
	const uint32_t override = DLC::getDlcCount(appId);
	if (override)
	{
		return override;
	}

	return count;
}

static bool hkClientApps_GetDLCDataByIndex(void* pClientApps, uint32_t appId, uint32_t dlcIndex, uint32_t* pDlcId, bool* pIsAvailable, char* pChDlcName, steamsize_t dlcNameLen)
{
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


	// The name buffer is only filled on success.
	g_pLog->debugOnce
	(
		"%s(%p, %u, %u, %p, %p, %s, %u) -> %i\n",

		Hooks::IClientApps_GetDLCDataByIndex.name.c_str(),
		pClientApps,
		appId,
		dlcIndex,
		reinterpret_cast<void*>(pDlcId),
		reinterpret_cast<void*>(pIsAvailable),
		ret && pChDlcName ? pChDlcName : "",
		dlcNameLen,
		ret
	);

	return ret;
}

static uint32_t hkClientFriends_GetFriendGamePlayed(void* pClientFriends, uint64_t steamId, GamePlayed_t* gamePlayed)
{
	LOG_TRACE("Calling original\n");
	const uint32_t ret = Hooks::IClientFriends_GetFriendGamePlayed.tramp.fn(pClientFriends, steamId, gamePlayed);

	const uint32_t realAppId = FakeAppIds::getRealAppIdForCurrentPipe();
	const uint32_t fakeAppId = FakeAppIds::getFakeAppId(realAppId);

	if (fakeAppId && fakeAppId == gamePlayed->appId)
	{
		LOG_DEBUG("Set friend GamePlayed from %u to %u\n", gamePlayed->appId, realAppId);
		gamePlayed->appId = realAppId;
	}

	//We do not log this function, it's basically useless since we don't want any SteamIds in the logs

	return ret;
}

static bool hkClientRemoteStorage_IsCloudEnabledForApp(void* pClientRemoteStorage, uint32_t appId)
{
	LOG_TRACE("Calling tramp\n");
	const bool enabled = Hooks::IClientRemoteStorage_IsCloudEnabledForApp.tramp.fn(pClientRemoteStorage, appId);
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
		g_pLog->debugOnce("Disabled cloud for %u\n", appId);
		return false;
	}

	return enabled;
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

static bool hkClientUser_BLoggedOn(void* pClientUser)
{
	LOG_TRACE("Calling original\n");
	const bool ret = Hooks::IClientUser_BLoggedOn.originalFn.fn(pClientUser);
	
	if (Misc::shouldFakeOffline())
	{
		return false;
	}

	return ret;
}

static uint32_t hkClientUser_BUpdateAppOwnershipTicket(void* pClientUser, AppId_t appId, bool staleOnly)
{
	const auto cached = Ticket::getCachedTicket(appId);
	CUser* user = getLocalUser();
	if (user != nullptr && user->isSubscribed(appId) && !cached.isValid())
	{
		staleOnly = false;
		LOG_DEBUG("Force re-requesting OwnershipInfo for %u\n", appId);
	}

	LOG_TRACE("Calling original\n");
	const uint32_t ret = Hooks::IClientUser_BUpdateAppOwnershipTicket.originalFn.fn(pClientUser, appId, staleOnly);

	LOG_DEBUG
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
	const uint32_t size = Hooks::IClientUser_GetAppOwnershipTicketExtendedData.originalFn.fn(
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
	uint32_t* pOffAppId,
	uint32_t* pOffSteamId,
	uint32_t* pOffSig,
	uint32_t* pSigSize
)
{
	const uint32_t ret = Hooks::IClientUser_GetAppOwnershipTicketExtendedData.originalFn.fn(pClientUser, appId, pTicket, ticketSize, pOffAppId, pOffSteamId, pOffSig, pSigSize);
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
			prepared, pTicket, ticketSize, pOffAppId, pOffSteamId, pOffSig, pSigSize))
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

	if (ret)
	{
		Ticket::getTicketOwnershipExtendedData(appId);
	}

	return ret;
}

static bool hkClientUser_GetEncryptedAppTicket(void* pClientUser, void* pTicket, uint32_t ticketSize, uint32_t* pTicketSize)
{
	LOG_TRACE("Calling original\n");
	const bool success = Hooks::IClientUser_GetEncryptedAppTicket.originalFn.fn(pClientUser, pTicket, ticketSize, pTicketSize);

	LOG_DEBUG
	(
		"%s(%p, %p, %u, %p) -> %u\n",

		Hooks::IClientUser_GetEncryptedAppTicket.name.c_str(),
		pClientUser,
		pTicket,
		ticketSize,
		reinterpret_cast<void*>(pTicketSize),
		success
	);

	return success;
}

static bool hkClientUser_GetLegacyCDKey(void* pClientUser, AppId_t appId, char* pChKey, uint32_t keySize)
{
	Apps::getLegacyCDKey(appId);

	return Hooks::IClientUser_GetLegacyCDKey.originalFn.fn(pClientUser, appId, pChKey, keySize);
}

static uint8_t hkClientUser_IsUserSubscribedAppInTicket(void* pClientUser, uint64_t steamId, AppId_t appId)
{
	const uint8_t ticketState = Hooks::IClientUser_IsUserSubscribedAppInTicket.originalFn.fn(pClientUser, steamId, appId);
	g_pLog->debug
	(
		"%s(%p, %u) -> %i\n",

		Hooks::IClientUser_IsUserSubscribedAppInTicket.name.c_str(),
		pClientUser,
		appId,
		ticketState
	);
	
	if (DLC::userSubscribedInTicket(appId))
	{
		return 0;
	}

	return ticketState;
}

static CSteamId hkClientUser_GetSteamId(void* pClientUser)
{
	const CSteamId steamId = Hooks::IClientUser_GetSteamId.originalFn.fn(pClientUser);
	// The universe is set before logon; the account id only after it.
	if (!g_currentSteamId.isSet() && steamId.isSet())
	{
		g_currentSteamId = steamId;
		StatsPolicy::setAccount(steamId.accountId());
	}
	if (!g_pSteamEngine)
	{
		return steamId;
	}
	const auto utils = g_pSteamEngine->getUtils();
	if (!utils)
	{
		return steamId;
	}

	//Never spoof inside the Steamclient
	const AppId_t realAppId = FakeAppIds::getRealAppIdForCurrentPipe();
	if (!realAppId)
	{
		return steamId;
	}

	const auto overrides = g_config.steamIdOverride.get();
	if (overrides.contains(realAppId))
	{
		const uint64_t& id64 = overrides.at(realAppId);
		if (id64)
		{
			return CSteamId(id64);
		}

		const auto cached = Ticket::getCachedTicket(realAppId);
		if (cached.isValid())
		{
			return cached.steamId;
		}

		g_pLog->infoOnce
		(
			"SteamIdOverride for %u is set with automatic mode, but no AppOwnershipTicket exists in cache! Falling through to normal operation\n",
			realAppId
		);
	}

	if (Ticket::oneTimeSteamIdSpoof.contains(realAppId))
	{
		const CSteamId newId = Ticket::oneTimeSteamIdSpoof.at(realAppId);
		Ticket::oneTimeSteamIdSpoof.erase(realAppId);

		return newId;
	}

	const auto ticket = Ticket::getCachedEncryptedTicket(realAppId);
	if (!ticket.isValid())
	{
		return steamId;
	}

	return ticket.steamId;
}

static void hkCGameInfoDialog_ServerResponded(void* pGameInfoDialog, gameserverdetails_t* details)
{
	FakeAppIds::pingResponse(details);
	Hooks::CGameInfoDialog_ServerResponded.tramp.fn(pGameInfoDialog, details);
}

static bool hkClientCompat_BIsCompatLayerEnabled(void* pClientCompat)
{
	if (!g_pClientCompat)
	{
		g_pClientCompat = reinterpret_cast<IClientCompat*>(pClientCompat);
		LOG_DEBUG("g_pClientCompat at %p\n", pClientCompat);
	}

	return Hooks::IClientCompat_BIsCompatLayerEnabled.tramp.fn(pClientCompat);
}

static bool hkClientConfigStore_SetString(void* pClientConfigStore, uint32_t store, const char* key, const char* value)
{
	LOG_TRACE("Calling tramp\n");
	const bool success = Hooks::IClientConfigStore_SetString.tramp.fn(pClientConfigStore, store, key, value);

	//LOG_DEBUG
	//(
	//	"%s(0x%x, %u, %s, %s) -> %u\n",

	//	Hooks::IClientConfigStore_SetString.name.c_str(),
	//	pClientConfigStore,
	//	store,
	//	key,
	//	value,
	//	success
	//);

	if (success)
	{
		Apps::setConfigStoreString(key, value);
	}

	return success;
}

namespace Hooks
{
	static bool depotOverrideHookReady = false;
	static bool serverBrowserHookReady = false;
	static bool serverDialogHookReady = false;
	static bool spawnGameHookReady = false;
	static bool cloudToggleHookReady = false;
	static bool compatLayerHookReady = false;
	static bool configStoreHookReady = false;
	static bool friendGameHookReady = false;
	static bool traceIpcHookReady = false;
	static bool routeMsgHookReady = false;
	static bool manifestRequestHookReady = false;

	DetourHook<TraceIPC_t> TraceIPC;


	DetourHook<CCMInterface_RecvPkt_t> CCMInterface_RecvPkt;

	DetourHook<CWebSocketConnection_BBuildAndAsyncSendFrame_t> CWebSocketConnection_BBuildAndAsyncSendFrame;
	DetourHook<CJobMgr_BRouteMsgToJob_t> CJobMgr_BRouteMsgToJob;
	DetourHook<CDepotDownloadMgr_BYldRequestDepotManifest_t> CDepotDownloadMgr_BYldRequestDepotManifest;

	DetourHook<CSteamMatchmakingServers_GetServerDetails_t> CSteamMatchmakingServers_GetServerDetails;
	DetourHook<CSteamMatchmakingServers_RequestInternetServerList_t> CSteamMatchmakingServers_RequestInternetServerList;

	DetourHook<CSteamEngine_ProcessIPCFrame_t> CSteamEngine_ProcessIPCFrame;
	DetourHook<CSteamEngine_SetAppIdForCurrentPipe_t> CSteamEngine_SetAppIdForCurrentPipe;

	DetourHook<CUser_CheckAppOwnership_t> CUser_CheckAppOwnership;
	DetourHook<CUser_GetSubscribedApps_t> CUser_GetSubscribedApps;
	DetourHook<CUserAppManager_BuildDepotDependency_t> CUserAppManager_BuildDepotDependency;
	DetourHook<CUser_PostCallbackToAppId_t> CUser_PostCallbackToAppId;
	DetourHook<CUser_SpawnGameId_t> CUser_SpawnGameId;

	DetourHook<IClientFriends_GetFriendGamePlayed_t> IClientFriends_GetFriendGamePlayed;

	VFTHook<IClientAppManager_BCanRemotePlayTogether_t> IClientAppManager_BCanRemotePlayTogether("IClientAppManager::BCanRemotePlayTogether");


	VFTHook<IClientAppManager_BIsDlcEnabled_t> IClientAppManager_BIsDlcEnabled("IClientAppManager::BIsDlcEnabled");
	VFTHook<IClientAppManager_GetAppUpdateInfo_t> IClientAppManager_GetAppUpdateInfo("IClientAppManager::GetAppUpdateInfo");
	VFTHook<IClientAppManager_GetAppStateInfo_t> IClientAppManager_GetAppStateInfo;
	VFTHook<IClientAppManager_LaunchApp_t> IClientAppManager_LaunchApp("IClientAppManager::LaunchApp");
	VFTHook<IClientAppManager_IsAppDlcInstalled_t> IClientAppManager_IsAppDlcInstalled("IClientAppManager::IsAppDlcInstalled");

	VFTHook<IClientApps_GetDLCDataByIndex_t> IClientApps_GetDLCDataByIndex("IClientApps::GetDLCDataByIndex");
	VFTHook<IClientApps_GetDLCCount_t> IClientApps_GetDLCCount("IClientApps::GetDLCCount");

	DetourHook<IClientCompat_BIsCompatLayerEnabled_t> IClientCompat_BIsCompatLayerEnabled;
	DetourHook<IClientConfigStore_SetString_t> IClientConfigStore_SetString;

	DetourHook<IClientRemoteStorage_IsCloudEnabledForApp_t> IClientRemoteStorage_IsCloudEnabledForApp;

	VFTHook<IClientUser_BLoggedOn_t> IClientUser_BLoggedOn;
	VFTHook<IClientUser_BUpdateAppOwnershipTicket_t> IClientUser_BUpdateAppOwnershipTicket;
	VFTHook<IClientUser_GetAppOwnershipTicketExtendedData_t> IClientUser_GetAppOwnershipTicketExtendedData;
	VFTHook<IClientUser_IsUserSubscribedAppInTicket_t> IClientUser_IsUserSubscribedAppInTicket;
	VFTHook<IClientUser_GetEncryptedAppTicket_t> IClientUser_GetEncryptedAppTicket("IClientUser::GetEncryptedAppTicket");
	VFTHook<IClientUser_GetLegacyCDKey_t> IClientUser_GetLegacyCDKey;
	VFTHook<IClientUser_GetSteamId_t> IClientUser_GetSteamId;

	VFTHook<IClientUtils_GetAppId_t> IClientUtils_GetAppId("IClientUtils::GetAppId");
	VFTHook<IClientUtils_GetOfflineMode_t> IClientUtils_GetOfflineMode("IClientUtils::GetOfflineMode");

	DetourHook<CGameInfoDialog_ServerResponded_t> CGameInfoDialog_ServerResponded;
}

void Hooks::placeVFTHooks()
{
	static std::atomic<bool> placed { false };
	if (placed.load(std::memory_order_acquire))
	{
		return;
	}

	{
		static std::mutex mutex;
		const std::lock_guard guard(mutex);
		if (placed.load(std::memory_order_relaxed) || !g_pSteamEngine)
		{
			return;
		}

		// The interface objects are members of the local user (and of the engine
		// for IClientUtils), located by offset instead of being captured from the
		// IClient*::RunIPCFrame dispatchers (upstream 8de3384).
		CUser* const user = g_pSteamEngine->getUser();
		IClientUtils* const utils = g_pSteamEngine->getUtils();
		if (!user || !utils)
		{
			return;
		}

		g_pClientAppManager = user->getAppManager();
		g_pClientApps = user->getClientApps();
		g_pClientUtils = utils;

		// The interfaces are packed SDK classes (alignment 1); their first word is
		// the vtable pointer either way (upstream 2710ef9).
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
		{
			auto vft = std::make_shared<lm_vmt_t>();
			LM_VmtNew(*reinterpret_cast<lm_address_t**>(g_pClientAppManager), vft.get());
			IClientAppManager_BCanRemotePlayTogether.setup(vft, VFTIndexes::IClientAppManager::BCanRemotePlayTogether, hkClientAppManager_BCanRemotePlayTogether);
			IClientAppManager_BIsDlcEnabled.setup(vft, VFTIndexes::IClientAppManager::BIsDlcEnabled, hkClientAppManager_BIsDlcEnabled);
			IClientAppManager_GetAppUpdateInfo.setup(vft, VFTIndexes::IClientAppManager::GetUpdateInfo, hkClientAppManager_GetUpdateInfo);
			IClientAppManager_GetAppStateInfo.setup(vft, VFTIndexes::IClientAppManager::GetAppStateInfo, hkClientAppManager_GetAppStateInfo);
			IClientAppManager_LaunchApp.setup(vft, VFTIndexes::IClientAppManager::LaunchApp, hkClientAppManager_LaunchApp);
			IClientAppManager_IsAppDlcInstalled.setup(vft, VFTIndexes::IClientAppManager::IsAppDlcInstalled, hkClientAppManager_IsAppDlcInstalled);
			IClientAppManager_BCanRemotePlayTogether.place();
			IClientAppManager_BIsDlcEnabled.place();
			IClientAppManager_GetAppUpdateInfo.place();
			IClientAppManager_GetAppStateInfo.place();
			IClientAppManager_LaunchApp.place();
			IClientAppManager_IsAppDlcInstalled.place();
			g_pLog->debug("IClientAppManager->vft at %p\n", static_cast<void*>(vft->vtable));
		}
		{
			auto vft = std::make_shared<lm_vmt_t>();
			LM_VmtNew(*reinterpret_cast<lm_address_t**>(g_pClientApps), vft.get());
			IClientApps_GetDLCDataByIndex.setup(vft, VFTIndexes::IClientApps::GetDLCDataByIndex, hkClientApps_GetDLCDataByIndex);
			IClientApps_GetDLCCount.setup(vft, VFTIndexes::IClientApps::GetDLCCount, hkClientApps_GetDLCCount);
			IClientApps_GetDLCDataByIndex.place();
			IClientApps_GetDLCCount.place();
			g_pLog->debug("IClientApps->vft at %p\n", static_cast<void*>(vft->vtable));
		}
		{
			IClientUser* const clientUser = user->getClientUser();
			auto vft = std::make_shared<lm_vmt_t>();
			LM_VmtNew(*reinterpret_cast<lm_address_t**>(clientUser), vft.get());
			IClientUser_BLoggedOn.setup(vft, VFTIndexes::IClientUser::BLoggedOn, &hkClientUser_BLoggedOn);
			IClientUser_BUpdateAppOwnershipTicket.setup(vft, VFTIndexes::IClientUser::BUpdateAppOwnershipTicket, hkClientUser_BUpdateAppOwnershipTicket);
			IClientUser_GetAppOwnershipTicketExtendedData.setup(vft, VFTIndexes::IClientUser::GetAppOwnershipTicketExtendedData, hkClientUser_GetAppOwnershipTicketExtendedData);
			IClientUser_IsUserSubscribedAppInTicket.setup(vft, VFTIndexes::IClientUser::IsUserSubscribedAppInTicket, &hkClientUser_IsUserSubscribedAppInTicket);
			IClientUser_GetEncryptedAppTicket.setup(vft, VFTIndexes::IClientUser::GetEncryptedAppTicket, hkClientUser_GetEncryptedAppTicket);
			IClientUser_GetLegacyCDKey.setup(vft, VFTIndexes::IClientUser::GetLegacyCDKey, hkClientUser_GetLegacyCDKey);
			IClientUser_GetSteamId.setup(vft, VFTIndexes::IClientUser::GetSteamID, hkClientUser_GetSteamId);
			IClientUser_BLoggedOn.place();
			IClientUser_BUpdateAppOwnershipTicket.place();
			IClientUser_GetAppOwnershipTicketExtendedData.place();
			IClientUser_IsUserSubscribedAppInTicket.place();
			IClientUser_GetEncryptedAppTicket.place();
			IClientUser_GetLegacyCDKey.place();
			IClientUser_GetSteamId.place();
			g_pLog->debug("IClientUser->vft at %p\n", static_cast<void*>(vft->vtable));
		}
		{
			auto vft = std::make_shared<lm_vmt_t>();
			LM_VmtNew(*reinterpret_cast<lm_address_t**>(utils), vft.get());
			IClientUtils_GetAppId.setup(vft, VFTIndexes::IClientUtils::GetAppId, hkClientUtils_GetAppId);
			IClientUtils_GetOfflineMode.setup(vft, VFTIndexes::IClientUtils::GetOfflineMode, hkClientUtils_GetOfflineMode);
			IClientUtils_GetAppId.place();
			IClientUtils_GetOfflineMode.place();
			g_pLog->debug("IClientUtils->vft at %p\n", static_cast<void*>(vft->vtable));
		}
		#pragma GCC diagnostic pop

		placed.store(true, std::memory_order_release);
	}

	// Upstream's SLSsteam::initialized point: CUser and its interfaces exist.
	// Fired once, by the thread that placed the hooks, outside the lock.
	Lua::fireCallback(Lua::Callbacks::SLSsteam_Initialized);
}

bool Hooks::setup()
{
	g_pLog->debug("Hooks::setup()\n");

	bool succeeded =
		CCMInterface_RecvPkt.setup(VFTIndexes::CCMInterface::RecvPkt, &hkCMInterface_RecvPkt)

		&& CWebSocketConnection_BBuildAndAsyncSendFrame.setup(Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame, &ManifestCode::hkBBuildAndAsyncSendFrame)
		&& CUser_CheckAppOwnership.setup(Patterns::CUser::CheckAppOwnership, &hkUser_CheckAppOwnership)
		&& CUser_GetSubscribedApps.setup(Patterns::CUser::GetSubscribedApps, &hkUser_GetSubscribedApps)
		&& CUser_PostCallbackToAppId.setup(Patterns::CUser::PostCallbackToAppId, &hkUser_PostCallbackToAppId)


		&& CSteamEngine_ProcessIPCFrame.setup(Patterns::CSteamEngine::ProcessIPCFrame, &hkSteamEngine_ProcessIPCFrame)
		&& CSteamEngine_SetAppIdForCurrentPipe.setup(Patterns::CSteamEngine::SetAppIdForCurrentPipe, &hkSteamEngine_SetAppIdForCurrentPipe);

	// Only the manifest request-code fallback rides these two detours, and
	// each works alone: BYldRequestDepotManifest waits for a staged blob before
	// Steam's own request, BRouteMsgToJob feeds GetManifestRequestCode answers
	// to the donor (x86-64 reads those in the CM receive hook instead).
	routeMsgHookReady =
		CJobMgr_BRouteMsgToJob.setup(Patterns::CJobMgr::BRouteMsgToJob, &ManifestCode::hkBRouteMsgToJob);
	manifestRequestHookReady =
		CDepotDownloadMgr_BYldRequestDepotManifest.setup(Patterns::CDepotDownloadMgr::BYldRequestDepotManifest, &ManifestCode::hkCDepotDownloadMgr_BYldRequestDepotManifest);
	if (!manifestRequestHookReady)
	{
		g_pLog->warn("BYldRequestDepotManifest hook unavailable; manifest request fallback disabled\n");
	}
	// TraceIPC only logs IPC calls under ExtendedLogging.
	traceIpcHookReady = TraceIPC.setup(Patterns::TraceIPC, &hkTraceIPC);
	if (!traceIpcHookReady)
	{
		g_pLog->warn("TraceIPC hook unavailable; ExtendedLogging IPC trace disabled\n");
	}

	// The server-browser hooks sit on fixed vtable slots that are validated
	// against a layout anchor (vftableinfo.cpp). A shifted layout disables
	// FakeAppIds in the server browser instead of every hook.
	serverBrowserHookReady =
		CSteamMatchmakingServers_GetServerDetails.setup(VFTIndexes::CSteamMatchmakingServers::GetServerDetails, &hkSteamMatchmakingServers_GetServerDetails)
		&& CSteamMatchmakingServers_RequestInternetServerList.setup(VFTIndexes::CSteamMatchmakingServers::RequestInternetServerList, &hkSteamMatchmakingServers_RequestInternetServerList);
	if (!serverBrowserHookReady)
	{
		g_pLog->warn("Server browser hooks unavailable; FakeAppIds in the server browser disabled\n");
	}
	serverDialogHookReady = CGameInfoDialog_ServerResponded.setup(
		VFTIndexes::CGameInfoDialog::ServerResponded, hkCGameInfoDialog_ServerResponded);
	if (!serverDialogHookReady)
	{
		g_pLog->warn("Game info dialog hook unavailable; FakeAppIds in server details disabled\n");
	}





	// Only the DepotBlacklist and ManifestIds keys ride this detour, so an
	// unresolved locator disables those two keys instead of every hook.
	depotOverrideHookReady = CUserAppManager_BuildDepotDependency.setup(
		Patterns::CUserAppManager::BuildDepotDependency, hkUserAppManager_BuildDepotDependency);
	if (!depotOverrideHookReady)
	{
		g_pLog->warn(
			"BuildDepotDependency hook unavailable; DepotBlacklist and ManifestIds disabled\n");
	}
	// Only LaunchOptions rides CUser::SpawnGameId (upstream 1bbcbc8), so an
	// unresolved locator disables that key instead of every hook.
	spawnGameHookReady = CUser_SpawnGameId.setup(
		Patterns::CUser::SpawnGameId, hkUser_SpawnGameId);
	if (!spawnGameHookReady)
	{
		g_pLog->warn("SpawnGameId hook unavailable; LaunchOptions disabled\n");
	}

	// The cloud toggle is detoured on CUserRemoteStorage's own slot (upstream
	// 269e867). CloudRedirect patches that vtable slot, so its hook runs first
	// and calls through this detour. Only DisableCloud rides it.
	{
		const auto& entry = VFTIndexes::IClientRemoteStorage::IsCloudEnabledForApp;
		const auto storage = Decompiler::vftables.find("18CUserRemoteStorage");
		if (storage != Decompiler::vftables.end())
		{
			const auto& slots = storage->second.functions;
			cloudToggleHookReady = entry.index < slots.size()
				&& IClientRemoteStorage_IsCloudEnabledForApp.setup(
					entry.getPrintName().c_str(), slots[entry.index],
					hkClientRemoteStorage_IsCloudEnabledForApp);
		}
	}
	if (!cloudToggleHookReady)
	{
		g_pLog->warn("IsCloudEnabledForApp hook unavailable; DisableCloud disabled\n");
	}

	// CCompatManager's BIsCompatLayerEnabled slot captures the compatibility
	// manager for the API's getcompat/setcompat (upstream b7407e4). Only those
	// commands ride it.
	{
		const auto& entry = VFTIndexes::IClientCompat::BIsCompatLayerEnabled;
		const auto compat = Decompiler::vftables.find("14CCompatManager");
		if (compat != Decompiler::vftables.end())
		{
			const auto& slots = compat->second.functions;
			compatLayerHookReady = entry.index < slots.size()
				&& IClientCompat_BIsCompatLayerEnabled.setup(
					entry.getPrintName().c_str(), slots[entry.index],
					hkClientCompat_BIsCompatLayerEnabled);
		}
	}
	if (!compatLayerHookReady)
	{
		g_pLog->warn("BIsCompatLayerEnabled hook unavailable; API compat commands disabled\n");
	}
	// The private app list is read from CConfigStore's own SetString slot
	// (upstream b8ef92f), which also sees stores that skip the IPC proxy.
	// Only the presence title redaction rides it.
	{
		const auto& entry = VFTIndexes::IClientConfigStore::SetString;
		const auto store = Decompiler::vftables.find("12CConfigStore");
		if (store != Decompiler::vftables.end())
		{
			const auto& slots = store->second.functions;
			configStoreHookReady = entry.index < slots.size()
				&& IClientConfigStore_SetString.setup(
					entry.getPrintName().c_str(), slots[entry.index],
					hkClientConfigStore_SetString);
		}
	}
	if (!configStoreHookReady)
	{
		g_pLog->warn("ConfigStore SetString hook unavailable; private apps are not redacted\n");
	}

	// Friends' games are translated back from FakeAppIds on CUserFriends' own
	// slot (upstream 50eed0d): game processes reach the implementation over
	// IPC, never the in-process IClientFriendsMap proxy. Only the lobby
	// translation rides it.
	{
		const auto& entry = VFTIndexes::IClientFriends::GetFriendGamePlayed;
		const auto friends = Decompiler::vftables.find("12CUserFriends");
		if (friends != Decompiler::vftables.end())
		{
			const auto& slots = friends->second.functions;
			friendGameHookReady = entry.index < slots.size()
				&& IClientFriends_GetFriendGamePlayed.setup(
					entry.getPrintName().c_str(), slots[entry.index],
					hkClientFriends_GetFriendGamePlayed);
		}
	}
	if (!friendGameHookReady)
	{
		g_pLog->warn("GetFriendGamePlayed hook unavailable; FakeAppIds lobbies keep the fake id\n");
	}

	// Nothing is placed after a required locator failed, and a required hook
	// that cannot be placed fails the load, which then runs Steam unmodified.
	if (!succeeded || !Hooks::place())
		return false;

	// AppInfoState keeps a non-owning Store* on its hot path: this Store, and the
	// one HotReload rebinds to, must outlive every detour and in-flight reader.
	static HotReloadState::Store bootstrapStore;
	(void)AppInfoState::setup(bootstrapStore);

	PackagePatch::setup();
	(void)LibraryRemoval::setup();
	ManifestBind::setup();
	DepotQuarantine::setup();
	ReconcilePin::setup();
	Parental::setup();
	return true;
}

bool Hooks::place()
{
	// Marked before the first hook goes in, so Hooks::remove() closes the
	// owner work queue whenever hooks may be live (OwnerQueue::PlacementGate).
	OwnerWork::notePlacement();
	// An optional hook that fails only loses its feature; place() warns.
	if (traceIpcHookReady)
		TraceIPC.place();
	bool required = CCMInterface_RecvPkt.place();

	if (routeMsgHookReady)
		CJobMgr_BRouteMsgToJob.place();
	if (manifestRequestHookReady)
		CDepotDownloadMgr_BYldRequestDepotManifest.place();
	required &= CSteamEngine_ProcessIPCFrame.place();
	required &= CSteamEngine_SetAppIdForCurrentPipe.place();

	if (serverBrowserHookReady)
	{
		CSteamMatchmakingServers_GetServerDetails.place();
		CSteamMatchmakingServers_RequestInternetServerList.place();
	}

	required &= CUser_CheckAppOwnership.place();
	required &= CUser_GetSubscribedApps.place();
	if (depotOverrideHookReady)
	{
		CUserAppManager_BuildDepotDependency.place();
	}
	required &= CUser_PostCallbackToAppId.place();
	if (spawnGameHookReady)
	{
		CUser_SpawnGameId.place();
	}
	if (cloudToggleHookReady)
	{
		IClientRemoteStorage_IsCloudEnabledForApp.place();
	}

	if (friendGameHookReady)
	{
		IClientFriends_GetFriendGamePlayed.place();
	}

	required &= CWebSocketConnection_BBuildAndAsyncSendFrame.place();



	if (serverDialogHookReady)
	{
		CGameInfoDialog_ServerResponded.place();
	}

	if (compatLayerHookReady)
	{
		IClientCompat_BIsCompatLayerEnabled.place();
	}
	if (configStoreHookReady)
	{
		IClientConfigStore_SetString.place();
	}
	return required;
}

void Hooks::remove()
{
	// Close the owner-thread work queue FIRST: after this nothing new is
	// accepted and anything pending is abandoned, so unhooking can never race
	// a fresh watcher-originated Steam-owned call in.
	//
	// Only once hooks were placed; shutdownIfPlaced() is idempotent, and a
	// teardown before placement leaves the queue accepting work.
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

	CCMInterface_RecvPkt.remove();

	CWebSocketConnection_BBuildAndAsyncSendFrame.remove();
	CJobMgr_BRouteMsgToJob.remove();
	CDepotDownloadMgr_BYldRequestDepotManifest.remove();
	CSteamEngine_ProcessIPCFrame.remove();
	CSteamEngine_SetAppIdForCurrentPipe.remove();

	if (serverBrowserHookReady)
	{
		CSteamMatchmakingServers_GetServerDetails.remove();
		CSteamMatchmakingServers_RequestInternetServerList.remove();
		serverBrowserHookReady = false;
	}

	CUser_CheckAppOwnership.remove();
	CUser_GetSubscribedApps.remove();
	if (depotOverrideHookReady)
	{
		CUserAppManager_BuildDepotDependency.remove();
		depotOverrideHookReady = false;
	}
	CUser_PostCallbackToAppId.remove();
	if (spawnGameHookReady)
	{
		CUser_SpawnGameId.remove();
		spawnGameHookReady = false;
	}

	if (friendGameHookReady)
	{
		IClientFriends_GetFriendGamePlayed.remove();
		friendGameHookReady = false;
	}

	IClientAppManager_BCanRemotePlayTogether.remove();


	IClientUser_BLoggedOn.remove();
	IClientUser_BUpdateAppOwnershipTicket.remove();
	IClientUser_GetAppOwnershipTicketExtendedData.remove();
	IClientUser_GetEncryptedAppTicket.remove();
	IClientUser_GetLegacyCDKey.remove();
	IClientUser_GetSteamId.remove();
	IClientUser_IsUserSubscribedAppInTicket.remove();

	if (serverDialogHookReady)
	{
		CGameInfoDialog_ServerResponded.remove();
		serverDialogHookReady = false;
	}
	if (compatLayerHookReady)
	{
		IClientCompat_BIsCompatLayerEnabled.remove();
		compatLayerHookReady = false;
	}
	if (configStoreHookReady)
	{
		IClientConfigStore_SetString.remove();
		configStoreHookReady = false;
	}

	IClientAppManager_BIsDlcEnabled.remove();
	IClientAppManager_GetAppUpdateInfo.remove();
	IClientAppManager_GetAppStateInfo.remove();
	IClientAppManager_LaunchApp.remove();
	IClientAppManager_IsAppDlcInstalled.remove();

	IClientApps_GetDLCDataByIndex.remove();
	IClientApps_GetDLCCount.remove();

	if (cloudToggleHookReady)
	{
		IClientRemoteStorage_IsCloudEnabledForApp.remove();
		cloudToggleHookReady = false;
	}

	IClientUtils_GetAppId.remove();
	IClientUtils_GetOfflineMode.remove();

	PackagePatch::remove();
	ManifestBind::remove();
	DepotQuarantine::remove();
	ReconcilePin::remove();
	Parental::remove();
}
