// CLog formats are printf formats that the compiler checks against their
// arguments. The Makefile target also compiles each LOG_FORMAT_* case below
// with the build's warning flags and expects the compiler to reject it.
#include "log.hpp"

#include <cstdio>
#include <string>

#if defined(LOG_FORMAT_MISMATCH)
// An argument that does not match its conversion.
void rejected(CLog& log) { log.info("app=%s\n", 7u); }
#elif defined(LOG_FORMAT_NONLITERAL)
// Run-time text used as the format; it has to go through "%s".
void rejected(CLog& log, const std::string& text) { log.warn(text.c_str()); }
#elif defined(LOG_FORMAT_CUSTOM)
// LOG_CUSTOM takes its flags first; the format after them is checked too.
void rejected(CLog& log) { log.custom(k_ELogLevelInfo, "app=%u\n", "x"); }
#endif

namespace
{
int failures = 0;
void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++failures;
}
}

int main()
{
	check(CLog::format("app=%u %s\n", 7u, "ready") == "app=7 ready\n",
	      "a message with arguments is formatted");
	check(CLog::format("%s", "progress 100%s done\n") == "progress 100%s done\n",
	      "text passed as an argument is copied verbatim");
	check(CLog::format("%s", "").empty(), "an empty result stays empty");
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
