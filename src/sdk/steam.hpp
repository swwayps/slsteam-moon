#pragma once

#include <cstdint>

typedef uint32_t AppId_t;
typedef uint32_t EMsg;
typedef uint64_t GameId_t;

namespace Steam
{
	typedef void*(*Plat_Alloc_t)(int);
	typedef void(*Plat_Free_t)(void*);
	typedef void*(*Plat_Realloc_t)(void*, int);

	extern Plat_Alloc_t Plat_Alloc;
	extern Plat_Free_t Plat_Free;
	extern Plat_Realloc_t Plat_Realloc;

	template<typename T>
	T* alloc(int size)
	{
		return reinterpret_cast<T*>(Plat_Alloc(size));
	}

	void free(void* mem);

	template<typename T>
	T* realloc(void* mem, int size)
	{
		return reinterpret_cast<T*>(Plat_Realloc(mem, size));
	}

	bool init();
}
