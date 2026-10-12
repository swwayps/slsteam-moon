// Log rotation at Steam start (make test-log-rotate).
#include "log_rotate.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
namespace
{
int failures = 0;
void check(bool ok, const std::string& label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label.c_str());
	if (!ok) ++failures;
}
void put(const std::string& path, const std::string& text)
{
	std::ofstream(path, std::ios::trunc) << text;
}
std::string get(const std::string& path)
{
	std::ifstream in(path);
	if (!in) return "<missing>";
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}
}
int main()
{
	const std::string dir = "/tmp/slssteam-log-rotate." + std::to_string(getpid());
	std::filesystem::create_directories(dir);
	const std::string log = dir + "/.SLSsteam.log";

	put(log, "small");
	LogRotate::rotate(log, 16, 3);
	check(get(log) == "small" && get(log + ".1") == "<missing>",
	      "a log below the limit is kept as it is");

	put(log, "0123456789abcdef");
	put(log + ".1", "one");
	put(log + ".2", "two");
	put(log + ".3", "three");
	LogRotate::rotate(log, 16, 3);
	check(get(log) == "<missing>", "a log at the limit moves aside");
	check(get(log + ".1") == "0123456789abcdef", "the current log becomes .1");
	check(get(log + ".2") == "one" && get(log + ".3") == "two",
	      "older generations shift up by one");
	check(get(log + ".4") == "<missing>", "the oldest generation is dropped");

	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	put(log, "0123456789abcdef");
	LogRotate::rotate(log, 16, 3);
	check(get(log + ".1") == "0123456789abcdef" && get(log + ".2") == "<missing>",
	      "the first rotation leaves a single generation");

	bool threw = false;
	try { LogRotate::rotate(dir + "/missing.log", 16, 3); }
	catch (...) { threw = true; }
	check(!threw, "a missing log is left alone without throwing");

	check(LogRotate::kMaxBytes == 8u << 20 && LogRotate::kKeep == 3,
	      "Steam start keeps up to 3 older logs and rotates at 8 MiB");

	std::filesystem::remove_all(dir);
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
