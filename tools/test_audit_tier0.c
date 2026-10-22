/* Fixture for make test-audit-tier0: a stand-in libtier0_s.so (-DTIER0_LIB)
 * and the program that links it, so it lives in the base namespace. */
#ifdef TIER0_LIB
#include <stdlib.h>
void* Plat_Alloc(int size) { return malloc((size_t)size); }
void Plat_Free(void* p) { free(p); }
void* Plat_Realloc(void* p, int size) { return realloc(p, (size_t)size); }
#else
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
void* Plat_Alloc(int size);
void Plat_Free(void* p);
int main(void)
{
	Plat_Free(Plat_Alloc(1));
	printf("main %p\n", dlsym(RTLD_DEFAULT, "Plat_Alloc"));
	return 0;
}
#endif
