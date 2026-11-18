#include "process.hpp"

#include "sdk/CSteamEngine.hpp"

#include "config.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <memory>
#include <regex>
#include <unordered_set>


IExecutableFile::~IExecutableFile()
{
	if (file)
	{
		fclose(file);
	}
}

bool IExecutableFile::load(const std::string& filePath, const LogLevel logErrorLevel)
{
	path = filePath;
	file = fopen(path.c_str(), "r");
	errorLevel = logErrorLevel;

	if (!file)
	{
		logFailure("Failed to open %s!\n", path.c_str());
		return false;
	}

	//Capture the on-disk size up front (non-throwing overload) so every
	//header-derived offset/size can be range-checked before it is used to
	//size an allocation or a read.
	std::error_code sizeError;
	fileSize = std::filesystem::file_size(path, sizeError);
	if (sizeError)
	{
		fileSize = 0;
	}

	if (!checkMagic())
	{
		return false;
	}

	if (!parseSections())
	{
		return false;
	}

	return true;
}

std::vector<uint8_t> IExecutableFile::readSection(const SectionHdr_t& section)
{
	//offset/size come straight from on-disk headers. A corrupt or hostile
	//binary can advertise a multi-gigabyte section; resize() would then throw
	//bad_alloc/length_error or fread would block on a huge read. Reject
	//anything that does not fit inside the file.
	if (fileSize == 0
		|| section.offset > fileSize
		|| section.size > fileSize - section.offset)
	{
		logFailure("Section %s out of bounds (offset %llu size %llu file %llu)!\n",
			section.name.c_str(),
			static_cast<unsigned long long>(section.offset),
			static_cast<unsigned long long>(section.size),
			static_cast<unsigned long long>(fileSize));
		return { };
	}

	if (fseek(file, section.offset, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to section %s!\n", section.name.c_str());
		return { };
	}

	auto bytes = std::vector<uint8_t>();
	bytes.resize(section.size);

	if (fread(bytes.data(), bytes.size(), 1, file) < 1)
	{
		logFailure("Failed to read section %s!\n", section.name.c_str());
		return { };
	}

	return bytes;
}

std::unique_ptr<IExecutableFile> IExecutableFile::create(
	const std::string& path, const LogLevel logErrorLevel)
{
	std::unique_ptr<IExecutableFile> file = std::make_unique<CPortableExecutableFile>();
	if (file->load(path, logErrorLevel))
	{
		return file;
	}

	file = std::make_unique<CELFExecutableFile>();
	if (file->load(path, logErrorLevel))
	{
		return file;
	}

	return nullptr;
}

bool CPortableExecutableFile::checkMagic()
{
	if (fseek(file, 0, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to magic!\n");
		return false;
	}

	std::string magic;
	magic.resize(2);

	if (fread(magic.data(), magic.size(), 1, file) < 1)
	{
		logFailure("Failed to read e_magic!\n");
		return false;
	}

	if (magic != "MZ")
	{
		return false;
	}

	return true;
}

bool CPortableExecutableFile::parseSections()
{
	if (!checkMagic())
	{
		return false;
	}

	if (g_pLog) g_pLog->debug("Parsing PE %s\n", path.filename().c_str());

	if (fseek(file, 0x3C, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to e_lfanew!\n");
		return false;
	}

	uint32_t e_lfanew;
	if (fread(&e_lfanew, sizeof(e_lfanew), 1, file) < 1)
	{
		logFailure("Failed to read e_lfanew!\n");
		return false;
	}

	if (fseek(file, e_lfanew, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to e_lfanew!\n");
		return false;
	}

	uint8_t peHdr[8] { };

	if (fread(peHdr, sizeof(peHdr), 1, file) < 1)
	{
		logFailure("Failed to read NT_HEADER64!\n");
		return false;
	}

	const uint16_t machine = *reinterpret_cast<uint16_t*>(&peHdr[4]); //Bitness
	const uint16_t numberOfSections = *reinterpret_cast<uint16_t*>(&peHdr[6]);

	uint64_t sectionHdrsOffset = e_lfanew;

	if (machine == MACHINE_I386)
	{
		sectionHdrsOffset += PE_HEADER32_SIZE;
		if (g_pLog) g_pLog->debug("Parsing as 32 bit file\n");
	}
	else if (machine == MACHINE_X64)
	{
		sectionHdrsOffset += PE_HEADER64_SIZE;
		if (g_pLog) g_pLog->debug("Parsing as 64 bit file\n");
	}
	else
	{
		logFailure("Unknown machine %u!\n", machine);
		return false;
	}

	if (fseek(file, sectionHdrsOffset, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to section headers!\n");
		return false;
	}

	for (size_t i = 0; i < numberOfSections; i++)
	{
		uint8_t sectHdr[SECTION_HEADER_SIZE];

		if (fread(sectHdr, sizeof(sectHdr), 1, file) < 1)
		{
			logFailure("Failed to read section header %lu!\n", i);
			return false;
		}

		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wstringop-truncation"
		char name[SECTION_HEADER_NAME_SIZE + 1] { }; //Zero allocate buffer for null termination
		strncpy(name, reinterpret_cast<char*>(sectHdr), sizeof(name) - 1);
		#pragma GCC diagnostic pop

		const uint32_t rva = *reinterpret_cast<uint32_t*>(&sectHdr[0xC]);
		const uint32_t size = *reinterpret_cast<uint32_t*>(&sectHdr[0x10]);
		const uint32_t ptr = *reinterpret_cast<uint32_t*>(&sectHdr[0x14]);

		if (g_config.extendedLogging.copy())
		{
			if (g_pLog) g_pLog->debug("Section header %s at 0x%x with size 0x%x\n", name, ptr, size);
		}

		sections.emplace_back(SectionHdr_t { std::string(name, strnlen(name, sizeof(name))), rva, ptr, size });
	}

	return true;
}

bool CELFExecutableFile::parseElf32Headers(const Elf32_Ehdr& hdr)
{
	if (sizeof(Elf32_Shdr) < hdr.e_shentsize)
	{
		logFailure("hdr.e_shentsize < sizeof(Elf_Shdr)!\n");
		return false;
	}

	if (hdr.e_shnum == 0 || hdr.e_shstrndx >= hdr.e_shnum)
	{
		logFailure("ELF has no usable section header string table\n");
		return false;
	}

	//The section header table must fit inside the file; otherwise e_shnum is
	//bogus and shdrs.resize()/fread() would over-allocate or over-read.
	const std::uintmax_t shTableBytes =
		static_cast<std::uintmax_t>(hdr.e_shnum) * sizeof(Elf32_Shdr);
	if (fileSize == 0
		|| static_cast<std::uintmax_t>(hdr.e_shoff) > fileSize
		|| shTableBytes > fileSize - static_cast<std::uintmax_t>(hdr.e_shoff))
	{
		logFailure("ELF section header table out of bounds\n");
		return false;
	}

	auto shdrs = std::vector<Elf32_Shdr>();
	shdrs.resize(hdr.e_shnum);

	if (fseek(file, hdr.e_shoff, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to section headers\n");
		return false;
	}

	if (fread(shdrs.data(), sizeof(Elf32_Shdr), shdrs.size(), file) < shdrs.size())
	{
		logFailure("Failed to read section headers\n");
		return false;
	}

	const Elf32_Shdr& strHdr = shdrs[hdr.e_shstrndx];

	if (static_cast<std::uintmax_t>(strHdr.sh_offset) > fileSize
		|| static_cast<std::uintmax_t>(strHdr.sh_size)
			> fileSize - static_cast<std::uintmax_t>(strHdr.sh_offset))
	{
		logFailure("ELF section string table out of bounds\n");
		return false;
	}

	auto strSec = std::vector<char>();
	strSec.resize(strHdr.sh_size);

	if (fseek(file, strHdr.sh_offset, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to strHdr.sh_offset!\n");
		return false;
	}

	if (fread(strSec.data(), sizeof(unsigned char), strSec.size(), file) < strSec.size())
	{
		logFailure("Failed to read strHdr!\n");
		return false;
	}
	//Names are read as C strings; a table without a final NUL would run past it.
	strSec.push_back('\0');

	//LOG_DEBUG("strHdr name %u address 0x%x\n", strHdr.sh_name, strHdr.sh_offset);

	for (const auto& shdr : shdrs)
	{
		if (!shdr.sh_name)
		{
			//LOG_DEBUG("Skipping nameless section\n");
			continue;
		}

		if (shdr.sh_name >= strSec.size())
		{
			//sh_name points past the string table; skip rather than read OOB.
			continue;
		}

		const char* name = &strSec[shdr.sh_name];

		if (g_config.extendedLogging.copy())
		{
			if (g_pLog) g_pLog->debug("Section header name %s, address 0x%x, offset 0x%x\n", name, shdr.sh_addr, shdr.sh_offset);
		}

		sections.emplace_back(SectionHdr_t { name, shdr.sh_addr, shdr.sh_offset, shdr.sh_size });
	}

	return true;
}

bool CELFExecutableFile::parseElf64Headers(const Elf64_Ehdr& hdr)
{
	if (sizeof(Elf64_Shdr) < hdr.e_shentsize)
	{
		logFailure("hdr.e_shentsize < sizeof(Elf_Shdr)!\n");
		return false;
	}

	if (hdr.e_shnum == 0 || hdr.e_shstrndx >= hdr.e_shnum)
	{
		logFailure("ELF has no usable section header string table\n");
		return false;
	}

	//The section header table must fit inside the file; otherwise e_shnum is
	//bogus and shdrs.resize()/fread() would over-allocate or over-read.
	const std::uintmax_t shTableBytes =
		static_cast<std::uintmax_t>(hdr.e_shnum) * sizeof(Elf64_Shdr);
	if (fileSize == 0
		|| static_cast<std::uintmax_t>(hdr.e_shoff) > fileSize
		|| shTableBytes > fileSize - static_cast<std::uintmax_t>(hdr.e_shoff))
	{
		logFailure("ELF section header table out of bounds\n");
		return false;
	}

	auto shdrs = std::vector<Elf64_Shdr>();
	shdrs.resize(hdr.e_shnum);

	if (fseek(file, hdr.e_shoff, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to section headers\n");
		return false;
	}

	if (fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), file) < shdrs.size())
	{
		logFailure("Failed to read section headers\n");
		return false;
	}

	const Elf64_Shdr& strHdr = shdrs[hdr.e_shstrndx];

	if (static_cast<std::uintmax_t>(strHdr.sh_offset) > fileSize
		|| static_cast<std::uintmax_t>(strHdr.sh_size)
			> fileSize - static_cast<std::uintmax_t>(strHdr.sh_offset))
	{
		logFailure("ELF section string table out of bounds\n");
		return false;
	}

	auto strSec = std::vector<char>();
	strSec.resize(strHdr.sh_size);

	if (fseek(file, strHdr.sh_offset, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to strHdr.sh_offset!\n");
		return false;
	}

	if (fread(strSec.data(), sizeof(unsigned char), strSec.size(), file) < strSec.size())
	{
		logFailure("Failed to read strHdr!\n");
		return false;
	}
	//Names are read as C strings; a table without a final NUL would run past it.
	strSec.push_back('\0');

	//LOG_DEBUG("strHdr name %u address 0x%llx\n", strHdr.sh_name, strHdr.sh_offset);

	for (const auto& shdr : shdrs)
	{
		if (!shdr.sh_name)
		{
			//LOG_DEBUG("Skipping nameless section\n");
			continue;
		}

		if (shdr.sh_name >= strSec.size())
		{
			//sh_name points past the string table; skip rather than read OOB.
			continue;
		}

		const char* name = &strSec[shdr.sh_name];

		if (g_config.extendedLogging.copy())
		{
			if (g_pLog) g_pLog->debug("Section header name %s, address 0x%lx, offset 0x%lx\n", name, shdr.sh_addr, shdr.sh_offset);
		}

		sections.emplace_back(SectionHdr_t { name, shdr.sh_addr, shdr.sh_offset, shdr.sh_size });
	}

	return true;
}

bool CELFExecutableFile::checkMagic()
{
	uint8_t magic[4] { };

	if (fseek(file, 0, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to Magic!\n");
		return false;
	}

	if (fread(magic, sizeof(magic), 1, file) < 1)
	{
		logFailure("Failed to read magic!\n");
		return false;
	}
	
	if
	(
		magic[EI_MAG0] != ELFMAG0
		|| magic[EI_MAG1] != ELFMAG1
		|| magic[EI_MAG2] != ELFMAG2
		|| magic[EI_MAG3] != ELFMAG3
	)
	{
		return false;
	}

	return true;
}

bool CELFExecutableFile::parseSections()
{
	if (!checkMagic())
	{
		return false;
	}

	if (g_pLog) g_pLog->debug("Parsing ELF %s\n", path.filename().c_str());

	if (fseek(file, 0, SEEK_SET) != 0)
	{
		logFailure("Failed to seek to file beginning!\n");
		return false;
	}

	Elf64_Ehdr hdr64;
	if (fread(&hdr64, sizeof(hdr64), 1, file) < 1)
	{
		logFailure("Failed to read Elf header!\n");
		return false;
	}

	if (hdr64.e_ident[EI_CLASS] == ELFCLASS32 && (hdr64.e_machine == EM_386 || hdr64.e_machine == EM_ARM))
	{
		if (g_pLog) g_pLog->debug("Parsing as 32 bit file\n");

		//Headers are the same till e_entry. The 64bit version is longer
		//so we can just recast it
		Elf32_Ehdr hdr32 = *reinterpret_cast<Elf32_Ehdr*>(&hdr64);
		return parseElf32Headers(hdr32);
	}
	else if (hdr64.e_ident[EI_CLASS] == ELFCLASS64 && (hdr64.e_machine == EM_X86_64 || hdr64.e_machine == EM_AARCH64))
	{
		if (g_pLog) g_pLog->debug("Parsing as 64 bit file\n");
		return parseElf64Headers(hdr64);
	}
	else
	{
		logFailure("Unknown ELFCLASS/e_machine %u | %u!\n", hdr64.e_ident[EI_CLASS], hdr64.e_machine);
		return false;
	}

	return false;
}
