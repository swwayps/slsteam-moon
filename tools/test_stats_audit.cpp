// Run normally (fallback) and under LD_AUDIT=bin/SLSsteam.so (real bridge).
// This executable is deliberately NOT named steam: no client setup is run.
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>

extern "C" __attribute__((visibility("default")))
uint64_t slsteam_local_stats_epoch_v1(uint32_t, uint32_t, uint32_t) noexcept
{
	return 0x1122334455667788ULL;
}

int main(int argc, char**)
{
	using Query = uint64_t (*)(uint32_t, uint32_t, uint32_t);
	const auto fn = reinterpret_cast<Query>(dlsym(RTLD_DEFAULT, "slsteam_local_stats_epoch_v1"));
	const auto result = fn ? fn(123, 7, 0) : UINT64_MAX;
	const uint64_t expected = argc > 1 ? 0 : 0x1122334455667788ULL;
	const bool ok = result == expected;
	std::printf("%s: %s stats bridge (64-bit C ABI on ELF32)\n", ok ? "ok" : "FAIL",
	            argc > 1 ? "LD_AUDIT" : "fallback");
	return ok ? 0 : 1;
}
