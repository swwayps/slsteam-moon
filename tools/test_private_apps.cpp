// Private-app list behind the presence title redaction
// (Apps::setConfigStoreString). Build and run: make test-private-apps
#include "../src/feats/apps.hpp"
#include "../src/log.hpp"

#include <cstdio>
#include <memory>

namespace
{
constexpr const char* key = "WebStorage\\PrivateApps";
int failures = 0;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

// The value arrives from a Steam caller through the SetString detour, so a
// throw would unwind into the client.
bool setWithoutThrowing(const char* k, const char* value)
{
	try
	{
		Apps::setConfigStoreString(k, value);
		return true;
	}
	catch (...)
	{
		return false;
	}
}
}

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

int main()
{
	check(setWithoutThrowing(key, "[730,240]"), "a numeric list is accepted");
	check(Apps::privateApps.contains(730u) && Apps::privateApps.contains(240u),
	      "listed apps are private");

	check(setWithoutThrowing(key, "[440,abc,4294967296,620]"),
	      "non-numeric and out-of-range entries do not throw");
	check(Apps::privateApps.contains(440u) && Apps::privateApps.contains(620u),
	      "valid entries next to bad ones are kept");
	check(!Apps::privateApps.contains(730u), "a new list replaces the previous one");

	check(setWithoutThrowing(key, "[]"), "an empty list is accepted");
	check(!Apps::privateApps.contains(440u), "an empty list clears the private apps");

	check(setWithoutThrowing("WebStorage\\Other", "[10]") && !Apps::privateApps.contains(10u),
	      "other keys are ignored");
	check(setWithoutThrowing(key, nullptr) && setWithoutThrowing(nullptr, "[10]"),
	      "null strings are ignored");

	return failures ? 1 : 0;
}
