#pragma once

#include "steam.hpp"
#include <cstddef>
#include <cstdint>

class CUser;
class IClientCompat;
class IClientUtils;

class CServerPipe
{
public:
	uint8_t __pad0x0[0x8];		//0x0
	HSteamPipe pipe;			//0x8
	uint8_t __pad0xC[0x8];		//0xC
	uint32_t pid;				//0x14
	uint8_t __pad0x18[0x8];		//0x18
	HSteamUser user;			//0x21
};

static_assert(offsetof(CServerPipe, pipe) == 0x8);
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
