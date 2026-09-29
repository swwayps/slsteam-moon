// Generated legacy CD keys (Apps::makeLegacyCDKey, used by the GetLegacyCDKey
// hook). Build and run: make test-legacy-cdkey
#include "../src/feats/apps.hpp"
#include "../src/log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool hasKeyShape(const std::string& key)
{
	if (key.size() != 19)
	{
		return false;
	}

	for (std::size_t i = 0; i < key.size(); i++)
	{
		const char c = key[i];
		const bool dash = (i + 1) % 5 == 0;
		const bool valid = dash
			? c == '-'
			: (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!valid)
		{
			return false;
		}
	}
	return true;
}
}

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

int main()
{
	const std::string key = Apps::makeLegacyCDKey(12345678u, 298110u);
	check(hasKeyShape(key), "a key is four dash-separated groups of four A-Z/0-9");
	check(Apps::makeLegacyCDKey(12345678u, 298110u) == key,
	      "the same account and app always get the same key");
	check(Apps::makeLegacyCDKey(12345678u, 47780u) != key,
	      "another app gets another key");

	// steamclient.so and steamui.so draw from the C library generator, so
	// making a key must leave its sequence alone.
	std::srand(7);
	const int expected = std::rand();
	std::srand(7);
	(void)Apps::makeLegacyCDKey(12345678u, 298110u);
	check(std::rand() == expected, "making a key leaves the process rand() sequence alone");

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
