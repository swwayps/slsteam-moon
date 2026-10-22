// Regression test (make test-audit-tier0): Steam::init runs in the audit
// namespace but must bind the allocator of the libtier0_s.so the program
// already uses. A plain dlopen there maps a second copy, and a packet body
// Steam allocated would then be freed by a foreign allocator (upstream ff2f543).
#include "../src/log.hpp"
#include "../src/sdk/steam.hpp"

#include <link.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

extern "C" unsigned int la_version(unsigned int)
{
	return LAV_CURRENT;
}

extern "C" void la_preinit(uintptr_t*)
{
	const char* dir = std::getenv("TIER0_TEST_DIR");
	g_pLog = std::unique_ptr<CLog>(new CLog((std::string(dir ? dir : "/tmp") + "/test.log").c_str()));
	if (Steam::init())
	{
		std::printf("audit %p\n", reinterpret_cast<void*>(Steam::Plat_Alloc));
	}
	else
	{
		std::puts("audit init-failed");
	}
	std::fflush(stdout);
}
