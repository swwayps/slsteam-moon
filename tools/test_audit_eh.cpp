// Regression test (make test-audit-eh): an exception thrown and caught inside
// an audit module built with the release flags reaches its handler. Next to a
// second audit module the same throw aborts the process instead, which is why
// the wrapper runs SLSsteam as the only auditor.

#include <link.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

extern "C" unsigned int la_version(unsigned int)
{
	return LAV_CURRENT;
}

__attribute__((noinline)) static unsigned long parse(const char* text)
{
	return std::stoul(text);
}

extern "C" void la_preinit(uintptr_t*)
{
	const char* text = std::getenv("SLSSTEAM_EH_INPUT");
	try
	{
		std::printf("parsed %lu\n", parse(text ? text : "not-a-number"));
	}
	catch (const std::exception&)
	{
		std::fputs("caught\n", stdout);
	}
	std::fflush(stdout);
}
