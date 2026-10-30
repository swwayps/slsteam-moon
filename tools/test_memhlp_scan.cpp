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
	return failures == 0 ? 0 : 1;
}
