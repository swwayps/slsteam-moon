#include "steam.hpp"

#include "../log.hpp"

#include <dlfcn.h>

namespace Steam
{
	Plat_Alloc_t Plat_Alloc;
	Plat_Free_t Plat_Free;
	Plat_Realloc_t Plat_Realloc;
}

void Steam::free(void* mem)
{
	Plat_Free(mem);
}

bool Steam::init()
{
	void* tier0 = dlopen("libtier0_s.so", RTLD_NOW);
	if (!tier0)
	{
		return false;
	}

	Plat_Alloc = reinterpret_cast<Plat_Alloc_t>(dlsym(tier0, "Plat_Alloc"));
	Plat_Free = reinterpret_cast<Plat_Free_t>(dlsym(tier0, "Plat_Free"));
	Plat_Realloc = reinterpret_cast<Plat_Realloc_t>(dlsym(tier0, "Plat_Realloc"));

	if (!Plat_Alloc || !Plat_Free || !Plat_Realloc)
	{
		return false;
	}

	g_pLog->debug("Plat_Alloc at %p\n", Plat_Alloc);
	g_pLog->debug("Plat_Free at %p\n", Plat_Free);
	g_pLog->debug("Plat_Realloc at %p\n", Plat_Realloc);

	return true;
}
