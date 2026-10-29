// The logger writes a once() line and each *Once line one time, and the set
// that remembers *Once lines stays bounded for a whole Steam session.
// Build: make test-log-once
#include "../src/config.hpp"
#include "../src/log.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
static int g_failures = 0;
static void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++g_failures;
}
static int countLines(const std::string& path, const std::string& line)
{
	std::ifstream in(path);
	int n = 0;
	for (std::string l; std::getline(in, l);)
		n += l == line;
	return n;
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_log_once_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	g_config.logLevel.set(static_cast<unsigned int>(LogLevel::Once));

	g_pLog->once("boot\n");
	g_pLog->once("boot\n");
	check(countLines(logPath, "[Once] boot") == 1, "once writes a line one time");

	g_pLog->infoOnce("app %d\n", 0);
	g_pLog->infoOnce("app %d\n", 0);
	check(countLines(logPath, "[Info] app 0") == 1, "infoOnce drops a repeat");

	for (int i = 1; i < 5000; ++i)
		g_pLog->infoOnce("app %d\n", i);
	check(countLines(logPath, "[Info] app 4999") == 1, "infoOnce writes each new line");
	g_pLog->infoOnce("app %d\n", 0);
	check(countLines(logPath, "[Info] app 0") == 2,
	      "infoOnce forgets old lines instead of keeping every one");

	std::printf("%d failure(s)\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
