#include "steam.hpp"

#include "../log.hpp"

#include <dlfcn.h>


namespace Steam
{
	Plat_Alloc_t Plat_Alloc;
	Plat_Free_t Plat_Free;
	Plat_Realloc_t Plat_Realloc;
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
		LOG_ERROR("Didn't find all of Steam's platform allocators!\n");
		return false;
	}

	LOG_DEBUG("Plat_Alloc at %p\n", reinterpret_cast<void*>(Plat_Alloc));
	LOG_DEBUG("Plat_Free at %p\n", reinterpret_cast<void*>(Plat_Free));
	LOG_DEBUG("Plat_Realloc at %p\n", reinterpret_cast<void*>(Plat_Realloc));

	return true;
}
