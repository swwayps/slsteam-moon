// The interface-map walk decodes every slot of a vtable at preinit. It must
// look the code segment up once per walk, not once per slot, and an address
// that no mapped segment holds ends that walk instead of being read.
// Build: make test-decompiler-walk
#include "config.hpp"
#include "decompiler.hpp"
#include "log.hpp"
#include "sdk/CSteamEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
CConfig g_config;
CConfig::~CConfig() = default;
CSteamEngine* g_pSteamEngine = nullptr;
CServerPipe* CSteamEngine::getServerPipe(HSteamPipe) { return nullptr; }
static int g_segmentLookups = 0;
extern "C" lm_bool_t __real_LM_FindSegment(lm_address_t address, lm_segment_t* segment);
extern "C" lm_bool_t __wrap_LM_FindSegment(lm_address_t address, lm_segment_t* segment)
{
	++g_segmentLookups;
	return __real_LM_FindSegment(address, segment);
}
namespace
{
int failures = 0;
void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++failures;
}
__attribute__((noinline)) int slotA(int v) { return v * 3 + 1; }
__attribute__((noinline)) int slotB(int v) { return v ^ 0x55; }
}
int main()
{
	VFTable table{};
	for (int i = 0; i < 32; ++i)
		table.functions.push_back(reinterpret_cast<lm_address_t>(i % 2 ? &slotA : &slotB));
	Decompiler::vftables["8TestMap"] = table;
	g_segmentLookups = 0;
	(void)Decompiler::parseInterfaceMapBase("8TestMap");
	std::printf("segment lookups for 32 slots: %d\n", g_segmentLookups);
	check(g_segmentLookups <= 1, "one interface-map walk looks its code segment up once");

	const long page = sysconf(_SC_PAGESIZE);
	void* gone = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	const bool unmapped = gone != MAP_FAILED && munmap(gone, page) == 0;
	std::fflush(stdout);
	const pid_t child = fork();
	if (child == 0)
	{
		std::unordered_map<lm_address_t, unsigned int> refs;
		Decompiler::parseFunction(reinterpret_cast<lm_address_t>(gone), refs);
		std::_Exit(refs.empty() ? 0 : 1);
	}
	int status = -1;
	waitpid(child, &status, 0);
	check(unmapped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "an address no mapped segment holds ends the walk without reading it");

	check(Decompiler::extractHexNum("rdi, [rip - 0x8]") == 0x8,
	      "extractHexNum parses a single-digit RIP displacement");
	check(Decompiler::extractHexNum("rdi, [rip - 0x0]") == 0x0,
	      "extractHexNum parses a zero RIP displacement");
	check(Decompiler::extractHexNum("rsi, [rip - 0x1a2b]") == 0x1a2b,
	      "extractHexNum parses a multi-digit RIP displacement");
	check(Decompiler::extractHexNum("rdi, [rip - 0x]") == LM_ADDRESS_BAD,
	      "extractHexNum rejects an empty 0x prefix");

	std::string out = "stale";
	const unsigned char binary[] = {0x01, 0x00};
	(void)Decompiler::getString(reinterpret_cast<lm_address_t>(binary), &out);
	check(out.empty(), "getString clears outStr when no printable string is found");
	return failures == 0 ? 0 : 1;
}
