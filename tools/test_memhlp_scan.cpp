// A signature scan counts matches only in executable segments that overlap
// the target module; a segment that merely touches it (ends at its base or
// starts at its end) belongs to another library.
// Build: make test-memhlp-scan
#include "config.hpp"
#include "log.hpp"
#include "memhlp.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/mman.h>
#include <vector>
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
CConfig g_config;
CConfig::~CConfig() = default;
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
	alignas(64) static unsigned char buffer[192] = {};
	const unsigned char signature[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37};
	std::memcpy(buffer + 8, signature, sizeof(signature));    // previous library
	std::memcpy(buffer + 72, signature, sizeof(signature));   // the module
	std::memcpy(buffer + 136, signature, sizeof(signature));  // next library
	const auto base = reinterpret_cast<lm_address_t>(buffer);
	MemHlp::CodeSegments code;
	for (int i = 0; i < 3; ++i)
	{
		lm_segment_t segment{};
		segment.base = base + 64 * i;
		segment.size = 64;
		segment.end = segment.base + segment.size;
		code.segments.push_back(segment);
	}
	lm_module_t module{};
	module.base = base + 64;
	module.size = 64;
	module.end = module.base + module.size;
	std::size_t matches = 0;
	const lm_address_t found = MemHlp::patternScan(
		"DE AD BE EF 13 37", module, &matches, nullptr, &code);
	check(matches == 1 && found == base + 72,
	      "segments that only touch the module are not scanned");

	// Relocate a conditional branch stolen into a detour trampoline
	// (mirrors 18CUserRemoteStorage::IsCloudEnabledForApp's `je rel32`).
	auto* page = static_cast<unsigned char*>(mmap(
		nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	check(page != MAP_FAILED, "executable test page mapped");
	if (page != MAP_FAILED)
	{
		std::memset(page, 0xCC, 4096);
		// +0:  85 ff                   test edi, edi
		// +2:  48 89 f8                mov rax, rdi
		// +5:  48 89 f8                mov rax, rdi
		// +8:  0f 1f 00                nop dword ptr [rax]
		// +11: 0f 84 af 00 00 00       je +0xaf (target = 0x11 + 0xaf = 0xc0)
		// +17: b8 07 00 00 00 c3       mov eax, 7 ; ret
		// +0xc0: b8 2a 00 00 00 c3     mov eax, 42 ; ret
		const unsigned char prologue[] = {
			0x85, 0xFF, 0x48, 0x89, 0xF8, 0x48, 0x89, 0xF8,
			0x0F, 0x1F, 0x00, 0x0F, 0x84, 0xAF, 0x00, 0x00, 0x00,
			0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3,
		};
		const unsigned char takenTarget[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
		std::memcpy(page, prologue, sizeof(prologue));
		std::memcpy(page + 0xC0, takenTarget, sizeof(takenTarget));

		static auto hookStub = +[](int) -> int { return -1; };
		lm_address_t tramp = LM_ADDRESS_BAD;
		const auto targetAddr = reinterpret_cast<lm_address_t>(page);
		const lm_size_t patchSize = LM_HookCode(
			targetAddr, reinterpret_cast<lm_address_t>(hookStub), &tramp);
		check(patchSize == 17 && tramp != LM_ADDRESS_BAD,
		      "detour hook steals the 17-byte je rel32 prologue");
		check(MemHlp::fixRelativeInstructions(targetAddr, tramp, patchSize),
		      "fixRelativeInstructions relocates the stolen je rel32");
		const auto trampFn = reinterpret_cast<int (*)(int)>(tramp);
		check(trampFn(1) == 7 && trampFn(0) == 42,
		      "relocated trampoline reaches both fallthrough and branch targets");
		LM_UnhookCode(targetAddr, tramp, patchSize);
		munmap(page, 4096);
	}
	return failures == 0 ? 0 : 1;
}
