#pragma once


namespace Steam
{
	typedef void*(*Plat_Alloc_t)(int);
	typedef void(*Plat_Free_t)(void*);
	typedef void*(*Plat_Realloc_t)(void*, int);

	extern Plat_Alloc_t Plat_Alloc;
	extern Plat_Free_t Plat_Free;
	extern Plat_Realloc_t Plat_Realloc;

	bool init();
}
