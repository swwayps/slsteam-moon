// String settings read through the real CConfig::getSetting.
// Build and run: make test-config-strings
#include "../src/config.hpp"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
int failures = 0;

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
void CConfig::setError(const ELoadError, const char*) {}

int main()
{
	CConfig config;
	YAML::Node node = YAML::Load(
		"FakeName:\n"
		"FakeEmail: \"\"\n"
		"Title: Alice\n");

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

	return failures ? 1 : 0;
}
