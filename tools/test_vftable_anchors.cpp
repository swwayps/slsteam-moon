// Fixed-index vtable entries against a real Steam client: every anchored
// entry resolves, and a shifted layout fails closed (a mandatory entry fails
// the registry, an optional one only loses its address).
//
//   make test-vftable-anchors [STEAMCLIENT=... STEAMUI=...]
#include "config.hpp"
#include "decompiler.hpp"
#include "log.hpp"
#include "sdk/CSteamEngine.hpp"
#include "vftableinfo.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
CConfig g_config;
CConfig::~CConfig() = default;
CSteamEngine* g_pSteamEngine = nullptr;
CServerPipe* CSteamEngine::getServerPipe(HSteamPipe) { return nullptr; }

namespace
{
int failures = 0;

void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++failures;
}

// The module at its link-time layout, as la_objopen sees it before relocation.
bool mapModule(const char* path, const char* name, lm_module_t& mod)
{
	const int fd = open(path, O_RDONLY);
	struct stat st {};
	if (fd < 0 || fstat(fd, &st) != 0) return false;
	auto* file = static_cast<unsigned char*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
	close(fd);
	if (file == MAP_FAILED) return false;
	auto* eh = reinterpret_cast<Elf64_Ehdr*>(file);
	auto* ph = reinterpret_cast<Elf64_Phdr*>(file + eh->e_phoff);
	std::uint64_t lo = UINT64_MAX, hi = 0;
	for (int i = 0; i < eh->e_phnum; i++)
	{
		if (ph[i].p_type != PT_LOAD) continue;
		lo = std::min<std::uint64_t>(lo, ph[i].p_vaddr & ~std::uint64_t{0xfff});
		hi = std::max(hi, ph[i].p_vaddr + ph[i].p_memsz);
	}
	auto* image = static_cast<unsigned char*>(mmap(nullptr, hi - lo, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (image == MAP_FAILED) return false;
	for (int i = 0; i < eh->e_phnum; i++)
	{
		if (ph[i].p_type != PT_LOAD) continue;
		std::memcpy(image + (ph[i].p_vaddr - lo), file + ph[i].p_offset, ph[i].p_filesz);
	}
	mod = {};
	mod.base = reinterpret_cast<lm_address_t>(image) - lo;
	mod.end = mod.base + hi;
	mod.size = hi - lo;
	std::snprintf(mod.path, sizeof(mod.path), "%s", path);
	std::snprintf(mod.name, sizeof(mod.name), "%s", name);
	return true;
}

// Mirrors main.cpp decompileBeforeRelocation.
void decompile(const lm_module_t& mod)
{
	Decompiler::parseModule(mod);
	for (auto& [name, vft] : Decompiler::vftables)
		if (vft.moduleBase == mod.base)
			vft.analyze();
}
}

int main(int argc, char** argv)
{
	lm_module_t client {}, ui {};
	if (argc < 3 || !mapModule(argv[1], "steamclient.so", client) ||
	    !mapModule(argv[2], "steamui.so", ui))
	{
		std::printf("SKIP: no Steam client modules\n");
		return 0;
	}
	decompile(client);
	decompile(ui);

	check(VFTIndexes::init(), "the registry resolves on this client");
	check(VFTIndexes::CCMInterface::RecvPkt.address != 0,
	      "CCMInterface::RecvPkt passes its anchor");
	check(VFTIndexes::CGameInfoDialog::ServerResponded.address != 0,
	      "CGameInfoDialog::ServerResponded passes its anchor");
	check(VFTIndexes::CSteamMatchmakingServers::GetServerDetails.address != 0 &&
	      VFTIndexes::CSteamMatchmakingServers::RequestInternetServerList.address != 0,
	      "the matchmaking entries pass their layout anchor");

	// A shifted layout: the anchor string is no longer where the entry expects it.
	VFTableInfo_t shifted{"12CCMInterface", "RecvPkt", 2,
		{2, "CCMInterface - received heartbeat response, connection still good"}};
	check(!shifted.init() && shifted.address == 0,
	      "a shifted mandatory entry fails the registry");
	VFTableInfo_t shiftedOptional{"24CSteamMatchMakingServers", "GetServerDetails", 7,
		{9, "Invalid server type"}, true};
	check(shiftedOptional.init() && shiftedOptional.address == 0,
	      "a shifted optional entry only loses its address");

	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
