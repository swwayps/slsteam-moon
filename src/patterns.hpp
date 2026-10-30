#pragma once

#include "memhlp.hpp"

#include "libmem/libmem.h"

#include <string>
#include <vector>


struct Pattern_t
{
public:
	const std::string name;
	// Stable C++ registry identity used by published locator catalogs.  Runtime
	// display names are not sufficient because a few legacy entries deliberately
	// use the name of the Steam function they locate instead of their C++ symbol.
	const std::string symbol;
	const std::string pattern;
	const MemHlp::SigFollowMode followMode;
	std::vector<int16_t> prologue;

	// When true, a failure to resolve this pattern does NOT make
	// Patterns::init() fail (and therefore does not abort the whole
	// load).  Use for patterns that gate an optional, null-guarded
	// feature where "not found" must degrade to a safe no-op rather
	// than disabling SLSsteam entirely.
	bool optional = false;

	lm_address_t address = LM_ADDRESS_BAD;
	// The raw signature match is kept separately from the followed target so a
	// local cache can revalidate the exact bytes that produced the target RVA.
	lm_address_t matchAddress = LM_ADDRESS_BAD;
	lm_module_t* const module;

	Pattern_t(const char* name, const char* pattern,
	          MemHlp::SigFollowMode followMode, lm_module_t* module = nullptr,
	          const char* symbol = nullptr);
	Pattern_t(const char* name, const char* pattern,
	          MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue,
	          lm_module_t* module = nullptr, const char* symbol = nullptr);
	Pattern_t(const char* name, const char* pattern,
	          MemHlp::SigFollowMode followMode, std::vector<int16_t> prologue,
	          lm_module_t* module = nullptr, const char* symbol = nullptr);

	bool find(const MemHlp::CodeSegments& code);
};

namespace Patterns
{
	extern Pattern_t ParentalSignatureCheck;
	extern Pattern_t ParentalSettingsReceived;

	extern Pattern_t TraceIPC;

	namespace SteamUI
	{
		extern Pattern_t AppControllerRunFrame;
		extern Pattern_t GetAppByID;
		extern Pattern_t MarkAppChange;
		extern Pattern_t OwnershipFlagsReference;
	}

	namespace CSteamEngine
	{
		extern Pattern_t GetServerPipe;
		extern Pattern_t SetAppIdForCurrentPipe;
		extern Pattern_t ProcessIPCFrame;
		extern Pattern_t Offset_ClientUtils;
		extern Pattern_t Offset_User;
	}

	namespace CUser
	{
		extern Pattern_t CheckAppOwnership;
		extern Pattern_t GetSubscribedApps;
		extern Pattern_t PostCallback;
		extern Pattern_t PostCallbackToAppId;
		extern Pattern_t SpawnGameId;
		extern Pattern_t UpdateAppOwnershipTicket;
		extern Pattern_t MarkLicenseAsChanged;
		extern Pattern_t ProcessPendingLicenseUpdates;
		extern Pattern_t NotifyLicensesUpdated;
		extern Pattern_t m_OffsetClientUser;
		extern Pattern_t m_OffsetUserAppInfo;
		extern Pattern_t m_OffsetUserAppManager;
	}

	namespace CAppInfoCache
	{
		extern Pattern_t GetOrAddAppData;
		extern Pattern_t ThreadedReadFromDisk;
		extern Pattern_t SkipFlagReference;
		extern Pattern_t ShaReference;
	}

	namespace CUserAppManager
	{
		extern Pattern_t BuildDepotDependency;
	}

	namespace CPackageInfoCache
	{
		extern Pattern_t LoadPackage;
	}

	namespace CUtlMemory
	{
		extern Pattern_t Grow;
	}

	namespace CDepotDownloadMgr
	{
		extern Pattern_t ProcessDepotManifest;
		extern Pattern_t PrepareDepotDownload;
		extern Pattern_t BuildDepotDependency;
		extern Pattern_t EvaluateConfigChanges;
		extern Pattern_t OnChunkUnpackedStack;
		extern Pattern_t OnChunkUnpackedReg;
	}

	namespace IClientUtils
	{
		extern Pattern_t Offset_GetPipeIndex;
	}

	namespace CWebSocketConnection
	{
		extern Pattern_t BBuildAndAsyncSendFrame;
	}

	namespace CJobMgr
	{
		extern Pattern_t BRouteMsgToJob;
	}

	namespace CDepotDownloadMgr
	{
		extern Pattern_t BYldRequestDepotManifest;
	}


	// Construct-on-first-use accessor for the pattern registry.
	//
	// The global Pattern_t objects below register themselves into this
	// vector from their constructors.  A bare `std::vector` global would
	// be subject to the static-initialization-order fiasco: under some
	// compilers (e.g. the gcc 11 used for the portable container build)
	// the vector receives a *dynamic* initializer that runs AFTER the
	// Pattern_t constructors in the same translation unit, wiping every
	// entry they registered.  That left `patterns` empty, so init()
	// resolved nothing and every Pattern_t::address stayed 0, which made
	// Hooks::place() patch address 0 and segfault.  Wrapping the vector
	// in a function-local static guarantees it is initialized before the
	// first registrant runs, regardless of compiler or link order.
	std::vector<Pattern_t*>& patterns();
	bool init();
}
