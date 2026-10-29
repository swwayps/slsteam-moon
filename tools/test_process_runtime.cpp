#include "../src/sdk/types.hpp"
#include "../src/config.hpp"
#include "../src/log.hpp"
#include "../src/process.hpp"
#include "../src/utils.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

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
// parseSections() succeeds with an EMPTY section list, which readers must not
// index (a size_t underflow -> std::out_of_range).
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
// ~4 GiB. readSection() must not resize()/fread() a buffer sized past the
// file end.
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

// Parses a crafted file and reads every section it reports. Returns true when
// the parser stayed alive (returned without throwing).
bool parserSurvives(const char* path, const std::vector<uint8_t>& bytes)
{
	writeFile(path, bytes);
	try
	{
		const auto exe = IExecutableFile::create(path, LogLevel::Debug);
		if (!exe)
		{
			return true; // rejected outright is fine; the point is: no crash
		}
		for (const auto& section : exe->sections)
			(void)exe->readSection(section);
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

	const auto currentExecutable = IExecutableFile::create("/proc/self/exe");
	expect(currentExecutable != nullptr && !currentExecutable->sections.empty(),
	       "the upstream executable analyser parses the running ELF image");
	const auto missingExecutable = IExecutableFile::create(
		"/proc/self/definitely-missing", LogLevel::Debug);
	expect(missingExecutable == nullptr,
	       "a disappearing open file degrades to an ignored debug result");

	// The decompiler parses Steam's own modules with no exception barrier, so
	// a hostile or truncated file must degrade instead of aborting the client.

	expect(parserSurvives("/tmp/slsteam_zero_section.pe", makeZeroSectionPe()),
	       "a zero-section PE does not abort the section parser");

	expect(parserSurvives("/tmp/slsteam_stripped.elf64", makeStrippedElf64()),
	       "a section-header-stripped ELF does not abort the section parser");

	expect(parserSurvives("/tmp/slsteam_huge_strtab.elf64", makeHugeStringTableElf64()),
	       "an ELF section string table larger than the file is rejected without a giant allocation");

	expect(parserSurvives("/tmp/slsteam_huge_bind.pe", makeHugeBindSectionPe()),
	       "a section larger than the file does not trigger a giant read");
	expect(sectionNameStopsAtTableEnd("/tmp/slsteam_unterminated.elf32",
	           makeUnterminatedNamesElf<Elf32_Ehdr, Elf32_Shdr>(ELFCLASS32, 0x3)),
	       "an unterminated ELF32 section name stops at the string table end");
	expect(sectionNameStopsAtTableEnd("/tmp/slsteam_unterminated.elf64",
	           makeUnterminatedNamesElf<Elf64_Ehdr, Elf64_Shdr>(ELFCLASS64, 0x3e)),
	       "an unterminated ELF64 section name stops at the string table end");

	return failures == 0 ? 0 : 1;
}
