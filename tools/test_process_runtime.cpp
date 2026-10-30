#include "../src/sdk/types.hpp"
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/process.hpp"
#include "../src/utils.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// While set, large allocations (whole executable sections) fail, as on a game
// whose .text does not fit the 32-bit client's address space.
static bool g_failLargeAllocations = false;
void* operator new(std::size_t size)
{
	if (g_failLargeAllocations && size >= 10000) throw std::bad_alloc();
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
std::unique_ptr<CLog> g_pLog;
CConfig g_config;

CLog::CLog(const char* logPath) : path(logPath) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
bool CLog::shouldNotify() { return false; }
CConfig::~CConfig() = default;

namespace
{
int failures = 0;

// Reads /proc/<pid>/cmdline until it holds `expected`, so a check does not
// race the child's exec.
bool waitForCmdline(pid_t pid, const std::string& expected)
{
	const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
	for (int i = 0; i < 200; ++i)
	{
		std::ifstream in(path, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), {});
		if (bytes == expected)
		{
			return true;
		}
		usleep(10000);
	}
	return false;
}

void expect(bool condition, const char* message)
{
	if (condition)
	{
		std::printf("ok:   %s\n", message);
		return;
	}

	std::printf("FAIL: %s\n", message);
	++failures;
}

void writeFile(const char* path, const std::vector<uint8_t>& bytes)
{
	FILE* f = std::fopen(path, "wb");
	if (f)
	{
		std::fwrite(bytes.data(), 1, bytes.size(), f);
		std::fclose(f);
	}
}

// A valid-magic PE (machine i386) whose COFF header advertises zero sections.
// parseSections() succeeds with an EMPTY section list, so hasSteamDRM() must
// not index sections.at(size()-1) (a size_t underflow -> std::out_of_range).
std::vector<uint8_t> makeZeroSectionPe()
{
	std::vector<uint8_t> b(0x88, 0);
	b[0] = 'M'; b[1] = 'Z';
	const uint32_t eLfanew = 0x80;
	std::memcpy(&b[0x3C], &eLfanew, sizeof(eLfanew));
	b[0x80] = 'P'; b[0x81] = 'E'; b[0x82] = 0; b[0x83] = 0;
	const uint16_t machineI386 = 0x014c;
	const uint16_t numberOfSections = 0;
	std::memcpy(&b[0x84], &machineI386, sizeof(machineI386));
	std::memcpy(&b[0x86], &numberOfSections, sizeof(numberOfSections));
	return b;
}

// A valid-magic 64-bit ELF whose section header table was stripped
// (e_shnum == 0). This is common for DRM-packed / protected shipping
// binaries. The parser must not index an empty section-header vector.
std::vector<uint8_t> makeStrippedElf64()
{
	std::vector<uint8_t> b(sizeof(Elf64_Ehdr), 0);
	Elf64_Ehdr hdr{};
	hdr.e_ident[EI_MAG0] = ELFMAG0;
	hdr.e_ident[EI_MAG1] = ELFMAG1;
	hdr.e_ident[EI_MAG2] = ELFMAG2;
	hdr.e_ident[EI_MAG3] = ELFMAG3;
	hdr.e_ident[EI_CLASS] = ELFCLASS64;
	hdr.e_machine = 0x3e; // EM_X86_64, matches CELFExecutableFile::ISA_AMD64
	hdr.e_shoff = 0;
	hdr.e_shnum = 0;
	hdr.e_shstrndx = 0;
	std::memcpy(b.data(), &hdr, sizeof(hdr));
	return b;
}

// A valid-magic 64-bit ELF whose single section header advertises a ~4 GiB
// string table (sh_size = 0xFFFFFFFF) that cannot fit in the tiny file. The
// parser must reject it instead of strSec.resize()/fread()-ing 4 GiB sized
// straight from the header (a bad_alloc/length_error, or a real read that
// stalls).
std::vector<uint8_t> makeHugeStringTableElf64()
{
	std::vector<uint8_t> b(sizeof(Elf64_Ehdr) + sizeof(Elf64_Shdr), 0);
	Elf64_Ehdr hdr{};
	hdr.e_ident[EI_MAG0] = ELFMAG0;
	hdr.e_ident[EI_MAG1] = ELFMAG1;
	hdr.e_ident[EI_MAG2] = ELFMAG2;
	hdr.e_ident[EI_MAG3] = ELFMAG3;
	hdr.e_ident[EI_CLASS] = ELFCLASS64;
	hdr.e_machine = 0x3e; // EM_X86_64, matches CELFExecutableFile::ISA_AMD64
	hdr.e_shoff = sizeof(Elf64_Ehdr);
	hdr.e_shnum = 1;
	hdr.e_shstrndx = 0;
	hdr.e_shentsize = sizeof(Elf64_Shdr);
	std::memcpy(b.data(), &hdr, sizeof(hdr));

	Elf64_Shdr shdr{};
	shdr.sh_name = 0;
	shdr.sh_offset = 0;
	shdr.sh_size = 0xFFFFFFFFu;
	std::memcpy(b.data() + sizeof(Elf64_Ehdr), &shdr, sizeof(shdr));
	return b;
}

// A valid-magic ELF whose section header string table has no terminating NUL.
// Section 1 is named from offset 1, so its name runs to the end of the table.
// The table is 12 bytes, an i386 malloc chunk with no slack, so an unbounded
// read runs into the next chunk header instead of a lucky zero byte.
template<typename Ehdr, typename Shdr>
std::vector<uint8_t> makeUnterminatedNamesElf(unsigned char elfClass, uint16_t machine)
{
	constexpr char table[] = "Z.text_abcde"; // written without its NUL
	const size_t tableOffset = sizeof(Ehdr) + 2 * sizeof(Shdr);
	std::vector<uint8_t> b(tableOffset + sizeof(table) - 1, 0);
	Ehdr hdr{};
	hdr.e_ident[EI_MAG0] = ELFMAG0;
	hdr.e_ident[EI_MAG1] = ELFMAG1;
	hdr.e_ident[EI_MAG2] = ELFMAG2;
	hdr.e_ident[EI_MAG3] = ELFMAG3;
	hdr.e_ident[EI_CLASS] = elfClass;
	hdr.e_machine = machine;
	hdr.e_shoff = sizeof(Ehdr);
	hdr.e_shnum = 2;
	hdr.e_shstrndx = 0;
	hdr.e_shentsize = sizeof(Shdr);
	std::memcpy(b.data(), &hdr, sizeof(hdr));
	Shdr names{};
	names.sh_offset = tableOffset;
	names.sh_size = sizeof(table) - 1;
	Shdr text{};
	text.sh_name = 1;
	std::memcpy(b.data() + sizeof(Ehdr), &names, sizeof(names));
	std::memcpy(b.data() + sizeof(Ehdr) + sizeof(Shdr), &text, sizeof(text));
	std::memcpy(b.data() + tableOffset, table, sizeof(table) - 1);
	return b;
}
bool sectionNameStopsAtTableEnd(const char* path, const std::vector<uint8_t>& bytes)
{
	writeFile(path, bytes);
	const auto exe = IExecutableFile::create(path, LogLevel::Debug);
	if (exe && exe->sections.size() == 1 && exe->sections[0].name == ".text_abcde")
	{
		return true;
	}
	std::fprintf(stderr, "  %s: parsed=%d sections=%zu first name length=%zu\n", path,
	             exe != nullptr, exe ? exe->sections.size() : 0,
	             exe && !exe->sections.empty() ? exe->sections[0].name.size() : 0);
	return false;
}
// A valid-magic i386 PE with a single ".bind" section whose size advertises
// ~4 GiB. hasSteamDRM() reads the last section when it is named ".bind";
// readSection() must not resize()/fread() a buffer sized past the file end.
std::vector<uint8_t> makeHugeBindSectionPe()
{
	const size_t sectionHdrOffset = 0x80 + 0xf8; // e_lfanew + PE_HEADER32_SIZE
	std::vector<uint8_t> b(sectionHdrOffset + 0x28, 0);
	b[0] = 'M'; b[1] = 'Z';
	const uint32_t eLfanew = 0x80;
	std::memcpy(&b[0x3C], &eLfanew, sizeof(eLfanew));
	b[0x80] = 'P'; b[0x81] = 'E'; b[0x82] = 0; b[0x83] = 0;
	const uint16_t machineI386 = 0x014c;
	const uint16_t numberOfSections = 1;
	std::memcpy(&b[0x84], &machineI386, sizeof(machineI386));
	std::memcpy(&b[0x86], &numberOfSections, sizeof(numberOfSections));
	std::memcpy(&b[sectionHdrOffset], ".bind", 5);
	const uint32_t hugeSize = 0xFFFFFFFFu;
	const uint32_t rawPtr = 0;
	std::memcpy(&b[sectionHdrOffset + 0x10], &hugeSize, sizeof(hugeSize));
	std::memcpy(&b[sectionHdrOffset + 0x14], &rawPtr, sizeof(rawPtr));
	return b;
}

// Runs hasSteamDRM() on a crafted file and reports whether it stayed alive
// (returned without throwing). Returns true when the analyser survived.
bool drmProbeSurvives(const char* path, const std::vector<uint8_t>& bytes)
{
	writeFile(path, bytes);
	try
	{
		const auto exe = IExecutableFile::create(path, LogLevel::Debug);
		if (!exe)
		{
			return true; // rejected outright is fine; the point is: no crash
		}
		(void)exe->hasSteamDRM();
		return true;
	}
	catch (...)
	{
		return false;
	}
}
}

int main()
{
	g_pLog = std::make_unique<CLog>("/dev/null");

	Process_t vanished{};
	vanished.pid = std::numeric_limits<pid_t>::max();
	bool vanishedThrew = false;
	std::filesystem::path vanishedExe;
	try
	{
		vanishedExe = vanished.getRealExe();
	}
	catch (...)
	{
		vanishedThrew = true;
	}
	expect(!vanishedThrew && vanishedExe.empty(),
	       "a vanished process is reported without throwing");

	Process_t overflow{};
	overflow.exe = "/tmp/game";
	overflow.environ = "SteamAppId=999999999999999999999999999999";
	bool overflowThrew = false;
	AppId_t overflowAppId = 1;
	try
	{
		overflowAppId = overflow.getAppIdFromEnv();
	}
	catch (...)
	{
		overflowThrew = true;
	}
	expect(!overflowThrew && overflowAppId == 0,
	       "an overflowing SteamAppId is rejected without throwing");

	Process_t valid{};
	valid.exe = "/tmp/game";
	valid.environ = "SteamAppId=480";
	expect(valid.getAppIdFromEnv() == 480,
	       "a valid SteamAppId is parsed unchanged");

	Process_t embedded{};
	embedded.exe = "/tmp/game";
	embedded.environ = "NotSteamAppId=480\0SteamGameId=480";
	expect(embedded.getAppIdFromEnv() == 0,
	       "an embedded SteamAppId name is not accepted");

	Process_t afterOtherVariable{};
	afterOtherVariable.exe = "/tmp/game";
	constexpr char environment[] = "SteamGameId=480\0SteamAppId=480";
	afterOtherVariable.environ.assign(environment, sizeof(environment) - 1);
	expect(afterOtherVariable.getAppIdFromEnv() == 480,
	       "SteamAppId is found after another environment variable");

	const auto currentExecutable = IExecutableFile::create("/proc/self/exe");
	expect(currentExecutable != nullptr && !currentExecutable->sections.empty(),
	       "the upstream executable analyser parses the running ELF image");
	const auto missingExecutable = IExecutableFile::create(
		"/proc/self/definitely-missing", LogLevel::Debug);
	expect(missingExecutable == nullptr,
	       "a disappearing open file degrades to an ignored debug result");

	// /proc/<pid>/cmdline separates the arguments with NUL bytes.
	const pid_t child = fork();
	if (child == 0)
	{
		char* const argv[] = {const_cast<char*>("/bin/sleep"), const_cast<char*>("5"), nullptr};
		char* const envp[] = {const_cast<char*>("SteamAppId=480"), nullptr};
		execve("/bin/sleep", argv, envp);
		_exit(127);
	}
	Process_t game{};
	const bool initialised = child > 0
		&& waitForCmdline(child, std::string("/bin/sleep\0" "5\0", 13))
		&& game.init(child, 1);
	if (child > 0)
	{
		kill(child, SIGKILL);
		waitpid(child, nullptr, 0);
	}
	expect(initialised && game.cmdLine == std::vector<std::string>{"/bin/sleep", "5"},
	       "each command line argument is its own field");

	// The executable analysis runs out of memory: the pipe keeps its app id.
	{
		g_config.smartTickets.set(CConfig::k_ESmartTicketsDenuvo);
		const pid_t sleeper = fork();
		if (sleeper == 0)
		{
			char* const argv[] = {const_cast<char*>("/bin/sleep"), const_cast<char*>("5"), nullptr};
			char* const envp[] = {const_cast<char*>("SteamAppId=480"), nullptr};
			execve("/bin/sleep", argv, envp);
			_exit(127);
		}
		Process_t analysed{};
		bool kept = false;
		if (sleeper > 0 && waitForCmdline(sleeper, std::string("/bin/sleep\0" "5\0", 13)))
		{
			g_failLargeAllocations = true;
			kept = analysed.init(sleeper, 1);
			g_failLargeAllocations = false;
			kill(sleeper, SIGKILL);
			waitpid(sleeper, nullptr, 0);
		}
		g_config.smartTickets.set(0);
		expect(kept && analysed.appId == 480,
		       "a failed executable analysis keeps the pipe's app id");
	}

	// SmartTickets DRM analysis runs on every process that connects to the
	// Steam pipe (owned games included), with no exception barrier. A binary
	// that parses with zero sections must not abort the whole Steam client.
	g_config.smartTickets.set(CConfig::k_ESmartTicketsSteamDRM);

	expect(drmProbeSurvives("/tmp/slsteam_zero_section.pe", makeZeroSectionPe()),
	       "a zero-section PE does not abort SteamDRM analysis");

	expect(drmProbeSurvives("/tmp/slsteam_stripped.elf64", makeStrippedElf64()),
	       "a section-header-stripped ELF does not abort SteamDRM analysis");

	expect(drmProbeSurvives("/tmp/slsteam_huge_strtab.elf64", makeHugeStringTableElf64()),
	       "an ELF section string table larger than the file is rejected without a giant allocation");

	expect(drmProbeSurvives("/tmp/slsteam_huge_bind.pe", makeHugeBindSectionPe()),
	       "a .bind section larger than the file does not trigger a giant read");
	expect(sectionNameStopsAtTableEnd("/tmp/slsteam_unterminated.elf32",
	           makeUnterminatedNamesElf<Elf32_Ehdr, Elf32_Shdr>(ELFCLASS32, 0x3)),
	       "an unterminated ELF32 section name stops at the string table end");
	expect(sectionNameStopsAtTableEnd("/tmp/slsteam_unterminated.elf64",
	           makeUnterminatedNamesElf<Elf64_Ehdr, Elf64_Shdr>(ELFCLASS64, 0x3e)),
	       "an unterminated ELF64 section name stops at the string table end");

	return failures == 0 ? 0 : 1;
}
