#pragma once

#include "CUtl.hpp"
#include "steam.hpp"

#include <cstddef>
#include <cstdint>

class CUser;
class IClientCompat;
class IClientUtils;

class IProcessPipe { };
using CUtlString = char*;

class __attribute__((packed)) __attribute__((aligned(1))) CServerPipe
{
public:
	IProcessPipe* internalPipe;		//0x0
	IProcessPipe* singleProcessPipe;	//0x4
	uint32_t pipeHandle;				//0x8
	uint8_t __pad0xC[8];				//0xC
	int32_t pid;						//0x14
	int32_t threadId;					//0x18
	CUtlString processName;				//0x1C - Was empty on the stuff I tried, maybe it's defunct on linux?
	uint8_t __pad0x20[1];				//0x20
	int32_t userHandle;					//0x21
	uint8_t __pad0x25[7];				//0x25
	void* queueCallbackMsg;				//0x2C
	uint8_t __pad0x30[8];				//0x30
	uint32_t numQueuedCallbacks;		//0x38
	uint8_t __pad0x3C[20];				//0x3C
	CUtlVector<void> debugText;			//0x50
}; //0x60

static_assert(offsetof(CServerPipe, pipeHandle) == 0x8);
static_assert(offsetof(CServerPipe, pid) == 0x14);

class CSteamEngine
{
public:
	CServerPipe* getServerPipe(HSteamPipe pipe);
	CUser* getUser(uint32_t index);
	IClientUtils* getUtils();
	void setAppIdForCurrentPipe(uint32_t appId);
};

extern CSteamEngine* g_pSteamEngine;

// Fallback to the local CUser* captured from the CUser::CheckAppOwnership
// hook.  Needed because CSteamEngine::Init is a one-shot call that Steam
// makes during early bootstrap; under the LD_PRELOAD injection model our
// hooks are placed after that call already ran, so g_pSteamEngine can stay
// null.  CheckAppOwnership fires early and often with the same CUser* that
// getUser(0) would return, so it is a reliable, timing-independent source.
extern CUser* g_pLocalUser;

// Returns the local user (pipe 0), preferring the engine when available and
// falling back to the cached CheckAppOwnership user.  May return nullptr if
// neither source has been observed yet; callers MUST null-check.
CUser* getLocalUser();

// Resolve CCompatManager, which implements IClientCompat and is embedded in
// the local CUser. Returns null when the optional member locator drifted or
// the local user has not been observed yet.
IClientCompat* getLocalClientCompat();
