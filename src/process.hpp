#pragma once

#include "log.hpp"
#include "sdk/types.hpp"

#include <cstdint>
#include <elf.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>


struct SectionHdr_t
{
	std::string name;
	uint64_t rva;
	uint64_t offset;
	uint64_t size;
};

class IExecutableFile
{
public:
	std::filesystem::path path;
	FILE* file;
	//Size of the opened file. Header-derived offsets/sizes are validated
	//against this so a corrupt or hostile binary cannot drive a multi-gigabyte
	//allocation/read off unvalidated on-disk fields.
	std::uintmax_t fileSize = 0;
	std::vector<SectionHdr_t> sections;
	//We use this to not clutter the log with failed open file reads
	LogLevel errorLevel;

	virtual ~IExecutableFile();

	bool load(const std::string& filePath, LogLevel logErrorLevel = LogLevel::Warn);
	std::vector<uint8_t> readSection(const SectionHdr_t& section);

	bool hasSteamDRM();
	bool hasDenuvo();

	virtual bool checkMagic() = 0;
	virtual bool parseSections() = 0;

	static std::unique_ptr<IExecutableFile> create(
		const std::string& path, LogLevel logErrorLevel = LogLevel::Warn);

protected:
	__attribute__((format(printf, 2, 3)))
	void logFailure(const char* message, ...) const
	{
		if (!g_pLog) return;
		va_list args;
		va_start(args, message);
		g_pLog->vwrite(errorLevel == LogLevel::Debug ? LogLevel::Debug : LogLevel::Warn,
		               false, message, args);
		va_end(args);
	}
};

class CPortableExecutableFile : public IExecutableFile
{
public:

	constexpr static uint32_t MACHINE_I386 = 0x14c;
	constexpr static uint32_t MACHINE_X64 = 0x8664;

	constexpr static size_t PE_HEADER32_SIZE = 0xf8;
	constexpr static size_t PE_HEADER64_SIZE = 0x108;

	constexpr static size_t SECTION_HEADER_SIZE = 0x28;
	constexpr static size_t SECTION_HEADER_NAME_SIZE = 0x8;

	virtual bool checkMagic();
	virtual bool parseSections();
};

class CELFExecutableFile : public IExecutableFile
{
public:

	constexpr static uint32_t ISA_X86 = 0x3;
	constexpr static uint32_t ISA_AMD64 = 0x3e;

	constexpr static size_t ELF_HEADER32_SIZE = sizeof(Elf32_Ehdr);
	constexpr static size_t ELF_HEADER64_SIZE = sizeof(Elf64_Ehdr);

	constexpr static size_t ELF_SHEADER32_SIZE = sizeof(Elf32_Shdr);
	constexpr static size_t ELF_SHEADER64_SIZE = sizeof(Elf64_Shdr);

	bool parseElf32Headers(const Elf32_Ehdr& hdr);
	bool parseElf64Headers(const Elf64_Ehdr& hdr);

	virtual bool checkMagic();
	virtual bool parseSections();
};

struct Process_t
{
	pid_t pid = -1;
	std::filesystem::path exe = "";
	std::vector<std::string> cmdLine = { };
	std::string environ = "";

	AppId_t appId = 0;
	HSteamPipe pipeHandle = 0x0;

	bool steamDRM = false;
	bool denuvo = false;

	std::filesystem::path getPath(const char* fileName);
	std::string readFile(const char* fileName);

	AppId_t getAppIdFromEnv();
	std::unordered_set<std::filesystem::path> getOpenFiles();
	std::filesystem::path getRealExe();

	bool analyse();
	bool init(const pid_t pid, const HSteamPipe pipeHandle);
};

extern std::unordered_map<HSteamPipe, Process_t> g_processMap;
