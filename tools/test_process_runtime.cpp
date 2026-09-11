#include "../src/sdk/steam.hpp"
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/process.hpp"

#include <cstdio>
#include <limits>
#include <memory>

std::unique_ptr<CLog> g_pLog;
CConfig g_config;

CLog::CLog(const char* logPath) : path(logPath) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
bool CLog::shouldNotify() { return false; }
CConfig::~CConfig() = default;

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
	if (condition)
	{
		std::printf("ok:   %s\n", message);
		return;
	}

	std::printf("FAIL: %s\n", message);
	++failures;
}
}

int main()
{
	g_pLog = std::make_unique<CLog>("/dev/null");

	Process_t vanished{};
	vanished.pid = std::numeric_limits<pid_t>::max();
	bool vanishedThrew = false;
	std::filesystem::path vanishedExe;
	try
	{
		vanishedExe = vanished.getRealExe();
	}
	catch (...)
	{
		vanishedThrew = true;
	}
	expect(!vanishedThrew && vanishedExe.empty(),
	       "a vanished process is reported without throwing");

	Process_t overflow{};
	overflow.exe = "/tmp/game";
	overflow.environ = "SteamAppId=999999999999999999999999999999";
	bool overflowThrew = false;
	AppId_t overflowAppId = 1;
	try
	{
		overflowAppId = overflow.getAppIdFromEnv();
	}
	catch (...)
	{
		overflowThrew = true;
	}
	expect(!overflowThrew && overflowAppId == 0,
	       "an overflowing SteamAppId is rejected without throwing");

	Process_t valid{};
	valid.exe = "/tmp/game";
	valid.environ = "SteamAppId=480";
	expect(valid.getAppIdFromEnv() == 480,
	       "a valid SteamAppId is parsed unchanged");

	Process_t embedded{};
	embedded.exe = "/tmp/game";
	embedded.environ = "NotSteamAppId=480\0SteamGameId=480";
	expect(embedded.getAppIdFromEnv() == 0,
	       "an embedded SteamAppId name is not accepted");

	Process_t afterOtherVariable{};
	afterOtherVariable.exe = "/tmp/game";
	constexpr char environment[] = "SteamGameId=480\0SteamAppId=480";
	afterOtherVariable.environ.assign(environment, sizeof(environment) - 1);
	expect(afterOtherVariable.getAppIdFromEnv() == 480,
	       "SteamAppId is found after another environment variable");

	const auto currentExecutable = IExecutableFile::create("/proc/self/exe");
	expect(currentExecutable != nullptr && !currentExecutable->sections.empty(),
	       "the upstream executable analyser parses the running ELF image");
	const auto missingExecutable = IExecutableFile::create(
		"/proc/self/definitely-missing", LogLevel::Debug);
	expect(missingExecutable == nullptr,
	       "a disappearing open file degrades to an ignored debug result");

	return failures == 0 ? 0 : 1;
}
