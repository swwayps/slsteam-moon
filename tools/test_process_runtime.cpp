#include "../src/sdk/steam.hpp"
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/process.hpp"
#include "../src/utils.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

std::unique_ptr<CLog> g_pLog;
CConfig g_config;

CLog::CLog(const char* logPath) : path(logPath) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
bool CLog::shouldNotify() { return false; }
CConfig::~CConfig() = default;

// hasSteamDRM() references this only for a non-empty ".bind" section; the
// crafted fixtures below never reach it, so a stub keeps the linker happy
// without dragging in the whole Utils translation unit.
double Utils::calculateEntropy(const std::vector<uint8_t>&) { return 0.0; }

namespace
{
int failures = 0;

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

	// SmartTickets DRM analysis runs on every process that connects to the
	// Steam pipe (owned games included), with no exception barrier. A binary
	// that parses with zero sections must not abort the whole Steam client.
	g_config.smartTickets.set(CConfig::k_ESmartTicketsSteamDRM);

	expect(drmProbeSurvives("/tmp/slsteam_zero_section.pe", makeZeroSectionPe()),
	       "a zero-section PE does not abort SteamDRM analysis");

	expect(drmProbeSurvives("/tmp/slsteam_stripped.elf64", makeStrippedElf64()),
	       "a section-header-stripped ELF does not abort SteamDRM analysis");

	return failures == 0 ? 0 : 1;
}
