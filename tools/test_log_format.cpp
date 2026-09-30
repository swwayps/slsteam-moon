// CLog message formatting: a message without arguments is text, so a '%' in
// it is never read as a conversion; messages with arguments format as printf.
#include "log.hpp"

#include <cstdio>
#include <string>

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
	check(CLog::format("progress 100%s done\n") == "progress 100%s done\n",
	      "a message without arguments is logged verbatim");
	check(CLog::format("app=%u %s\n", 7u, "ready") == "app=7 ready\n",
	      "a message with arguments is formatted");
	check(CLog::format("").empty(), "an empty message stays empty");
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
