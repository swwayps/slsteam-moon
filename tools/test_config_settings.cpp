// Settings read through the real CConfig::getSetting.
// Build and run: make test-config-settings
#include "../src/config.hpp"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
int failures = 0;
std::vector<std::string> parseErrors;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}
}

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
CConfig::~CConfig() = default;
void CConfig::setError(const ELoadError err, const char* keyName)
{
	if (err == ELoadError::ParsingException)
	{
		parseErrors.emplace_back(keyName);
	}
}

int main()
{
	CConfig config;
	YAML::Node node = YAML::Load(
		"FakeName:\n"
		"FakeEmail: \"\"\n"
		"Title: Alice\n"
		"FakeWalletBalance: 500000000000000000\n"
		"GoodWallet: 4999\n"
		"DisableCloud: yess\n");

	// yaml-cpp reads an empty `Key:` as the string "null"; FakeName would then
	// rename the persona to "null".
	check(config.getSetting<std::string>(node, "FakeName", "").empty(),
	      "an empty FakeName: reads as the default");
	check(config.getSetting<std::string>(node, "FakeEmail", "").empty(),
	      "a quoted empty string stays empty");
	check(config.getSetting<std::string>(node, "Title", "") == "Alice",
	      "a set string is read");
	check(config.getSetting<std::string>(node, "Missing", "fallback") == "fallback",
	      "a missing key reads as the default");
	check(parseErrors.empty(), "valid and empty values report no parse error");

	// Bad scalars fall back without throwing (a throw here aborted the client
	// at startup) and name the key in the load errors.
	check(config.getSetting<int32_t>(node, "FakeWalletBalance", 0) == 0,
	      "an out-of-range integer reads as the default");
	check(config.getSetting<int32_t>(node, "GoodWallet", 0) == 4999,
	      "an in-range integer is read");
	check(config.getSetting<bool>(node, "DisableCloud", true),
	      "an unparsable bool reads as the default");
	check(parseErrors == std::vector<std::string>{"FakeWalletBalance", "DisableCloud"},
	      "unparsable values are reported by key");

	return failures ? 1 : 0;
}
