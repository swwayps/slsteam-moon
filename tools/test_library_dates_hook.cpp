// Link the real app-ownership handler; stub only its Steam/config boundaries.
#include "../src/config.hpp"
#include "../src/feats/apps.hpp"
#include "../src/sdk/CAppOwnershipInfo.hpp"
#include "../src/sdk/IClientApps.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace
{
constexpr uint32_t appId = 2050650;
constexpr uint32_t packageDate = 1577128970;
constexpr uint32_t realDate = 1700000000;
int failures = 0;
bool excluded = false;
IClientApps clientApps;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

CAppOwnershipInfo checkDate(int32_t packageId, uint32_t date,
                           bool ownsLicense = true)
{
	CAppOwnershipInfo info{};
	info.subId = packageId;
	info.purchaseTime = date;
	info.ownsLicense = ownsLicense;
	Apps::checkAppOwnership(appId, &info);
	return info;
}
}

CConfig g_config;
uint32_t g_currentSteamId = 7;
IClientApps* g_pClientApps = &clientApps;
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CConfig::~CConfig() = default;
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
bool CConfig::isAddedAppId(uint32_t id) { return addedAppIds.contains(id); }
bool CConfig::shouldExcludeAppId(uint32_t) { return excluded; }
uint32_t CConfig::getDenuvoGameOwner(uint32_t) { return 0; }
EAppType IClientApps::getAppType(uint32_t) { return APPTYPE_GAME; }

int main()
{
	Apps::applistRequested = true;
	g_config.addedAppIds.set({appId});
	check(checkDate(0, packageDate).purchaseTime == 0,
	      "a managed app without a recorded date never inherits package 0's date");
	check(checkDate(0, 1096588800).purchaseTime == 0,
	      "the reported 2004 package date is also removed");
	char pattern[] = "/tmp/slsteam_library_dates_hook_XXXXXX";
	const char* temp = ::mkdtemp(pattern);
	if (!temp) return 2;
	std::string error;
	constexpr uint32_t addedDate = 1780000000;
	check(g_config.libraryDates.refresh(temp, "", {appId}, addedDate, error),
	      "source discovery records the inclusion date");
	check(checkDate(0, packageDate).purchaseTime == addedDate,
	      "the real ownership handler publishes the recorded inclusion date");
	check(g_config.libraryDates.refresh(temp, "", {appId}, addedDate + 100, error)
	      && checkDate(0, packageDate).purchaseTime == addedDate,
	      "repeated queries and config reloads never advance that date");
	check(checkDate(-1, 0, false).purchaseTime == addedDate,
	      "an app gets its inclusion date before native metadata is resolved");
	constexpr uint32_t newerAppId = 1086940;
	g_config.addedAppIds.set({appId, newerAppId});
	check(g_config.libraryDates.refresh(temp, "", {appId, newerAppId}, addedDate + 200, error),
	      "a later game receives a separate inclusion date");
	CAppOwnershipInfo newer{};
	newer.subId = 0;
	newer.ownsLicense = true;
	newer.purchaseTime = packageDate;
	check(Apps::checkAppOwnership(newerAppId, &newer)
	      && newer.purchaseTime > checkDate(0, packageDate).purchaseTime,
	      "two games with the same package date now sort by actual inclusion order");
	check(checkDate(42, realDate).purchaseTime == realDate,
	      "a real license keeps its purchase date even for a managed app");
	check(checkDate(42, realDate, false).purchaseTime == realDate,
	      "an expired real license still keeps its purchase date");
	check(checkDate(-1, realDate).purchaseTime == realDate,
	      "ambiguous metadata with an existing license remains untouched");

	g_config.subscriptionTimestamps.set({{appId, realDate}});
	check(checkDate(0, packageDate).purchaseTime == realDate,
	      "an explicit date override takes precedence");
	g_config.subscriptionTimestamps.set({{appId, 0}});
	check(checkDate(0, packageDate).purchaseTime == 0,
	      "an explicit zero override remains unset");
	g_config.subscriptionTimestamps.set({});

	excluded = true;
	check(checkDate(0, packageDate).purchaseTime == packageDate,
	      "excluded apps remain untouched");
	excluded = false;
	g_config.addedAppIds.set({});
	g_config.playNotOwnedGames.set(true);
	check(checkDate(0, packageDate, false).purchaseTime == packageDate,
	      "the generic app path does not acquire a managed-library date");
	std::filesystem::remove_all(temp);
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
