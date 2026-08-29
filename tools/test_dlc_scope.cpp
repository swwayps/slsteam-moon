// Regression coverage for the runtime DLC ownership policy.
//
// This links the real feats/dlc.cpp and supplies only the Steam/config
// boundaries it reads.  A DLC from an ordinary owned game must retain
// Steam's answer; only DLCs discovered from a LuaTools-managed base app may
// receive the local ownership override.

#include "../src/config.hpp"
#include "../src/feats/apps.hpp"
#include "../src/feats/dlc.hpp"
#include "../src/sdk/CAppOwnershipInfo.hpp"
#include "../src/sdk/CSteamEngine.hpp"
#include "../src/sdk/CUser.hpp"
#include "../src/sdk/IClientUtils.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_set>

namespace
{
constexpr std::uint32_t kBaseApp = 250900;
constexpr std::uint32_t kManagedDlc = 1426300;
constexpr std::uint32_t kManualDlc = 1118010;
constexpr std::uint32_t kUnmanagedDlc = 401920;

int failures = 0;
std::uint32_t activeApp = kBaseApp;
std::unordered_set<std::uint32_t> subscribedAppIds{kBaseApp};
std::unordered_set<std::uint32_t> excludedAppIds;
bool localUserAvailable = true;

#define CHECK(condition, message)                                           \
	do {                                                                     \
		if (!(condition)) { std::printf("FAIL: %s\n", message); ++failures; } \
		else { std::printf("ok:   %s\n", message); }                         \
	} while (0)

IClientUtils clientUtils;
CUser localUser;
}

CConfig g_config;
IClientUtils* g_pClientUtils = &clientUtils;
CSteamEngine* g_pSteamEngine = nullptr;
CUser* g_pLocalUser = nullptr;

CConfig::~CConfig() = default;

bool CConfig::shouldExcludeAppId(std::uint32_t appId)
{
	return excludedAppIds.contains(appId);
}

std::uint32_t IClientUtils::getAppId()
{
	return activeApp;
}

CUser* getLocalUser()
{
	return localUserAvailable ? &localUser : nullptr;
}

bool CUser::isSubscribed(std::uint32_t appId)
{
	return subscribedAppIds.contains(appId);
}

bool Apps::unlockApp(std::uint32_t, CAppOwnershipInfo*)
{
	return true;
}

int main()
{
	Apps::setDiscoveredAppDlcIds({kManagedDlc});
	CHECK(Apps::ownershipOverrideAllowed(
	          true, false, false, false, false, false, false),
	      "a LuaTools-managed app remains eligible before type discovery");
	CHECK(!Apps::ownershipOverrideAllowed(
	          false, true, false, true, true, false, false),
	      "an unmanaged DLC is rejected before any ownership metadata changes");

	CHECK(!Apps::genericOwnershipOverrideAllowed(
	          true, false, true, true, false, false),
	      "global app ownership never overrides a known DLC");
	CHECK(!Apps::genericOwnershipOverrideAllowed(
	          true, false, false, false, false, false),
	      "global app ownership fails closed until the app type is known");
	CHECK(Apps::genericOwnershipOverrideAllowed(
	          true, false, true, false, false, false),
	      "global app ownership still accepts a known non-DLC when filtering is off");
	CHECK(!Apps::genericOwnershipOverrideAllowed(
	          true, false, true, false, true, false),
	      "automatic filtering still rejects a non-game app");
	CHECK(Apps::genericOwnershipOverrideAllowed(
	          true, false, true, false, true, true),
	      "automatic filtering still accepts games and applications");

	CHECK(!DLC::shouldUnlockDlc(kUnmanagedDlc),
	      "an unmanaged DLC keeps Steam's ownership result");
	CHECK(DLC::shouldUnlockDlc(kManagedDlc),
	      "a managed DLC is unlocked even when the base game is already owned");
	CHECK(DLC::isDlcEnabled(kBaseApp, kManagedDlc),
	      "the enabled query evaluates the DLC id, not its owned base app");

	std::uint32_t listedDlc = kUnmanagedDlc;
	bool available = false;
	DLC::makeDlcAvailable(listedDlc, &available);
	CHECK(!available,
	      "an unmanaged listed DLC keeps Steam's availability result");
	listedDlc = kManagedDlc;
	DLC::makeDlcAvailable(listedDlc, &available);
	CHECK(available,
	      "a managed listed DLC is marked available after Steam identifies it");

	CConfig::CDlcData manualDlcData;
	manualDlcData.parentId = kBaseApp;
	manualDlcData.dlcIds.emplace(kManualDlc, "Iceborne");
	g_config.dlcData.set({{kBaseApp, manualDlcData}});
	Apps::setConfiguredAppDlcIds(CConfig::selectConfiguredDlcIds(
		g_config.managedAppIds.get(), g_config.dlcData.get()));
	CHECK(DLC::getDlcCount(kBaseApp) == 0,
	      "DlcData cannot replace the list for an unmanaged base app");
	CHECK(!DLC::shouldUnlockDlc(kManualDlc),
	      "DlcData cannot authorize a DLC under an unmanaged parent");
	g_config.managedAppIds.set({kBaseApp});
	Apps::setConfiguredAppDlcIds(CConfig::selectConfiguredDlcIds(
		g_config.managedAppIds.get(), g_config.dlcData.get()));
	CHECK(DLC::getDlcCount(kBaseApp) == 1,
	      "DlcData remains available for a LuaTools-managed base app");
	std::uint32_t manualDlc = 0;
	bool manualAvailable = false;
	char manualName[32] = {};
	std::size_t manualNameLen = sizeof(manualName);
	CHECK(DLC::getDlcDataByIndex(
	          kBaseApp, 0, &manualDlc, &manualAvailable,
	          manualName, manualNameLen)
	      && manualDlc == kManualDlc && manualAvailable,
	      "managed DlcData can still publish its configured DLC");
	CHECK(DLC::shouldUnlockDlc(kManualDlc),
	      "managed DlcData authorizes an ID absent from discovered appinfo");
	manualAvailable = false;
	DLC::makeDlcAvailable(kManualDlc, &manualAvailable);
	CHECK(manualAvailable,
	      "configured DLC availability uses the same managed scope");
	excludedAppIds.insert(kManualDlc);
	manualAvailable = true;
	CHECK(DLC::getDlcDataByIndex(
	          kBaseApp, 0, &manualDlc, &manualAvailable,
	          manualName, manualNameLen) && !manualAvailable,
	      "explicit exclusion wins over configured enumeration availability");
	excludedAppIds.erase(kManualDlc);
	g_config.managedAppIds.set({});
	Apps::setConfiguredAppDlcIds(CConfig::selectConfiguredDlcIds(
		g_config.managedAppIds.get(), g_config.dlcData.get()));
	CHECK(!DLC::shouldUnlockDlc(kManualDlc),
	      "removing the managed parent revokes configured DLC scope");
	CHECK(DLC::shouldUnlockDlc(kManagedDlc),
	      "configured replacement does not erase discovered DLC scope");
	g_config.dlcData.set({});

	subscribedAppIds.insert(kManagedDlc);
	CHECK(!DLC::shouldUnlockDlc(kManagedDlc),
	      "an already-owned managed DLC keeps Steam's ownership result");
	subscribedAppIds.erase(kManagedDlc);

	excludedAppIds.insert(kManagedDlc);
	CHECK(!DLC::shouldUnlockDlc(kManagedDlc),
	      "an explicitly excluded managed DLC is not unlocked");
	excludedAppIds.clear();

	activeApp = 0;
	CHECK(!DLC::shouldUnlockDlc(kManagedDlc),
	      "no DLC is unlocked outside an active app context");

	activeApp = kBaseApp;
	localUserAvailable = false;
	CHECK(DLC::shouldUnlockDlc(kManagedDlc),
	      "managed DLC remains eligible before local ownership is available");

	std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
