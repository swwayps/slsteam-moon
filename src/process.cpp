#include "process.hpp"

#include "config.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>


IExecutableFile::~IExecutableFile()
{
	if (file)
	{
		fclose(file);
	}
}

bool IExecutableFile::load(const std::string filePath)
{
	path = filePath;
	file = fopen(path.c_str(), "r");

	if (!file)
	{
		LOG_ERROR("Failed to open %s!\n", path.c_str());
		return false;
	}

	if (!parseSections())
	{
		return false;
	}

	return true;
}

bool IExecutableFile::hasSteamDRM()
{
	const auto& last = sections.at(sections.size() - 1);

	if (last.name == ".bind")
	{
		const auto bytes = readSection(last);
		const double entropy = Utils::calculateEntropy(bytes);

		LOG_DEBUG("%s has entropy of %f\n", last.name.c_str(), entropy);

		if (entropy >= 7.0)
		{
			return true;
		}
	}

	return false;
}

bool IExecutableFile::hasDenuvo()
{
	double text = 0.0;
	double xtext = 0.0;

	for (const auto& sec : sections)
	{
		if (sec.name == ".text")
		{
			const auto bytes = readSection(sec);
			text = Utils::calculateEntropy(bytes);
			LOG_DEBUG(".text has entropy of %f\n", text);
		}

		else if (sec.name == ".xtext")
		{
			const auto bytes = readSection(sec);
			xtext = Utils::calculateEntropy(bytes);
			LOG_DEBUG(".xtext has entropy of %f\n", xtext);
		}
	}

	return text >= 7.0 || xtext >= 7.0;
}

std::vector<uint8_t> IExecutableFile::readSection(const SectionHdr_t& section)
{
	if (fseek(file, section.offset, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to section %s!\n", section.name.c_str());
		return { };
	}

	auto bytes = std::vector<uint8_t>();
	bytes.resize(section.size);

	if (fread(bytes.data(), bytes.size(), 1, file) < 1)
	{
		LOG_ERROR("Failed to read section %s!\n", section.name.c_str());
		return { };
	}

	return bytes;
}

bool CPortableExecutableFile::parseSections()
{
	std::string magic;
	magic.resize(2);

	if (fread(magic.data(), magic.size(), 1, file) < 1)
	{
		LOG_ERROR("Failed to read e_magic!\n");
		return false;
	}

	if (magic != "MZ")
	{
		LOG_ERROR("Uknown PE magic %s!\n", magic.c_str());
		return false;
	}

	if (fseek(file, 0x3C, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to e_lfanew!\n");
		return false;
	}

	uint32_t e_lfanew;
	if (fread(&e_lfanew, sizeof(e_lfanew), 1, file) < 1)
	{
		LOG_ERROR("Failed to read e_lfanew!\n");
		return false;
	}

	if (fseek(file, e_lfanew, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to e_lfanew!\n");
		return false;
	}

	uint8_t peHdr[8] { };

	if (fread(peHdr, sizeof(peHdr), 1, file) < 1)
	{
		LOG_ERROR("Failed to read NT_HEADER64!\n");
		return false;
	}

	const uint16_t machine = *reinterpret_cast<uint16_t*>(&peHdr[4]); //Bitness
	const uint16_t numberOfSections = *reinterpret_cast<uint16_t*>(&peHdr[6]);

	uint64_t sectionHdrsOffset = e_lfanew;

	if (machine == MACHINE_I386)
	{
		sectionHdrsOffset += PE_HEADER32_SIZE;
		LOG_DEBUG("Parsing as 32 bit file\n");
	}
	else if (machine == MACHINE_X64)
	{
		sectionHdrsOffset += PE_HEADER64_SIZE;
		LOG_DEBUG("Parsing as 64 bit file\n");
	}
	else
	{
		LOG_ERROR("Unknown machine %u!\n", machine);
		return false;
	}

	if (fseek(file, sectionHdrsOffset, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to section headers!\n");
		return false;
	}

	for (size_t i = 0; i < numberOfSections; i++)
	{
		uint8_t sectHdr[SECTION_HEADER_SIZE];

		if (fread(sectHdr, sizeof(sectHdr), 1, file) < 1)
		{
			LOG_ERROR("Failed to read section header %i!\n", i);
			return false;
		}

		char name[SECTION_HEADER_NAME_SIZE];
		strncpy(name, reinterpret_cast<char*>(sectHdr), sizeof(name));

		const uint32_t rva = *reinterpret_cast<uint32_t*>(&sectHdr[0xC]);
		const uint32_t size = *reinterpret_cast<uint32_t*>(&sectHdr[0x10]);
		const uint32_t ptr = *reinterpret_cast<uint32_t*>(&sectHdr[0x14]);

		LOG_DEBUG("Section header %s at 0x%x with size 0x%x\n", name, ptr, size);

		sections.emplace_back(SectionHdr_t { std::string(name, strnlen(name, sizeof(name))), rva, ptr, size });
	}

	return true;
}

bool CELFExecutableFile::parseElf32Headers(const Elf32_Ehdr& hdr)
{
	if (sizeof(Elf32_Shdr) < hdr.e_shentsize)
	{
		LOG_ERROR("hdr.e_shentsize < sizeof(Elf_Shdr)!\n");
		return false;
	}

	auto shdrs = std::vector<Elf32_Shdr>();
	shdrs.resize(hdr.e_shnum);

	if (fseek(file, hdr.e_shoff, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to section headers\n");
		return false;
	}

	if (fread(shdrs.data(), sizeof(Elf32_Shdr), shdrs.size(), file) < shdrs.size())
	{
		LOG_ERROR("Failed to read section headers\n");
		return false;
	}

	const Elf32_Shdr& strHdr = shdrs[hdr.e_shstrndx];
	auto strSec = std::vector<char>();
	strSec.resize(strHdr.sh_size);

	if (fseek(file, strHdr.sh_offset, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to strHdr.sh_offset!\n");
		return false;
	}

	if (fread(strSec.data(), sizeof(unsigned char), strSec.size(), file) < strSec.size())
	{
		LOG_ERROR("Failed to read strHdr!\n");
		return false;
	}

	LOG_DEBUG("strHdr name %u address 0x%x\n", strHdr.sh_name, strHdr.sh_offset);

	for (const auto& shdr : shdrs)
	{
		if (!shdr.sh_name)
		{
			LOG_DEBUG("Skipping nameless section\n");
			continue;
		}

		const char* name = &strSec[shdr.sh_name];
		LOG_DEBUG("Section header name %s, address 0x%x, offset 0x%x\n", name, shdr.sh_addr, shdr.sh_offset);
		sections.emplace_back(SectionHdr_t { name, shdr.sh_addr, shdr.sh_offset, shdr.sh_size });
	}

	return true;
}

bool CELFExecutableFile::parseElf64Headers(const Elf64_Ehdr& hdr)
{
	if (sizeof(Elf64_Shdr) < hdr.e_shentsize)
	{
		LOG_ERROR("hdr.e_shentsize < sizeof(Elf_Shdr)!\n");
		return false;
	}

	auto shdrs = std::vector<Elf64_Shdr>();
	shdrs.resize(hdr.e_shnum);

	if (fseek(file, hdr.e_shoff, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to section headers\n");
		return false;
	}

	if (fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), file) < shdrs.size())
	{
		LOG_ERROR("Failed to read section headers\n");
		return false;
	}

	const Elf64_Shdr& strHdr = shdrs[hdr.e_shstrndx];
	auto strSec = std::vector<char>();
	strSec.resize(strHdr.sh_size);

	if (fseek(file, strHdr.sh_offset, SEEK_SET) != 0)
	{
		LOG_ERROR("Failed to seek to strHdr.sh_offset!\n");
		return false;
	}

	if (fread(strSec.data(), sizeof(unsigned char), strSec.size(), file) < strSec.size())
	{
		LOG_ERROR("Failed to read strHdr!\n");
		return false;
	}

	LOG_DEBUG("strHdr name %u address 0x%llx\n", strHdr.sh_name, strHdr.sh_offset);

	for (const auto& shdr : shdrs)
	{
		if (!shdr.sh_name)
		{
			LOG_DEBUG("Skipping nameless section\n");
			continue;
		}

		const char* name = &strSec[shdr.sh_name];
		LOG_DEBUG("Section header name %s, address 0x%llx, offset 0x%llx\n", name, shdr.sh_addr, shdr.sh_offset);
		sections.emplace_back(SectionHdr_t { name, shdr.sh_addr, shdr.sh_offset, shdr.sh_size });
	}

	return true;
}

bool CELFExecutableFile::parseSections()
{
	FILE* file = fopen(path.c_str(), "r");
	if (!file)
	{
		LOG_ERROR("Failed to open file for parsing Elf headers!\n");
		return false;
	}

	Elf64_Ehdr hdr64;
	if (fread(&hdr64, sizeof(hdr64), 1, file) < 1)
	{
		LOG_ERROR("Failed to read Elf header!\n");
		return false;
	}

	if
	(
		hdr64.e_ident[EI_MAG0] != ELFMAG0
		|| hdr64.e_ident[EI_MAG1] != ELFMAG1
		|| hdr64.e_ident[EI_MAG2] != ELFMAG2
		|| hdr64.e_ident[EI_MAG3] != ELFMAG3
	)
	{
		LOG_ERROR("ELF magic unknown!\n");
		return false;
	}

	LOG_DEBUG("shsstrndx %u\n", hdr64.e_shstrndx);

	if (hdr64.e_ident[EI_CLASS] == ELFCLASS32)
	{
		LOG_DEBUG("Parsing %s as 32 bit file\n", path.c_str());

		//Headers are the same till e_entry. The 64bit version is longer
		//so we can just recast it
		Elf32_Ehdr hdr32 = *reinterpret_cast<Elf32_Ehdr*>(&hdr64);
		parseElf32Headers(hdr32);
	}
	else if (hdr64.e_ident[EI_CLASS] == ELFCLASS64)
	{
		LOG_DEBUG("Parsing %s as 64 bit file\n", path.c_str());
		parseElf64Headers(hdr64);
	}
	else
	{
		LOG_ERROR("Unknown ELFCLASS %u!\n", hdr64.e_ident[EI_CLASS]);
	}

	return true;
}

std::filesystem::path Process_t::getPath(const char* fileName)
{
	std::ostringstream pathSS;
	pathSS << "/proc/" << pid << "/" << fileName;
	return pathSS.str();
}

std::string Process_t::readFile(const char* fileName)
{
	const auto path = getPath(fileName);

	auto ifstream = std::ifstream(path);
	if (!ifstream.is_open())
	{
		g_pLog->warn("Failed to read %s!\n", path.c_str());
		return "";
	}

	std::string content = std::string(std::istreambuf_iterator(ifstream), {});
	return content;
}

AppId_t Process_t::getAppIdFromEnv()
{
	constexpr std::string_view prefix = "SteamAppId=";
	std::size_t valueStart = 0;
	for (;;)
	{
		valueStart = environ.find(prefix, valueStart);
		if (valueStart == std::string::npos || valueStart == 0 ||
			environ[valueStart - 1] == '\0')
		{
			break;
		}
		valueStart += prefix.size();
	}
	if (valueStart == std::string::npos)
	{
		g_pLog->warn("No SteamAppId in %s's environment! Using 0\n", exe.filename().c_str());
		return 0;
	}

	const char* begin = environ.data() + valueStart + prefix.size();
	const char* end = static_cast<const char*>(
		std::memchr(begin, '\0', environ.data() + environ.size() - begin));
	if (!end)
	{
		end = environ.data() + environ.size();
	}

	AppId_t appId = 0;
	const auto result = std::from_chars(begin, end, appId);
	if (result.ec != std::errc{} || result.ptr != end)
	{
		g_pLog->warn("Invalid SteamAppId in %s's environment! Using 0\n", exe.filename().c_str());
		return 0;
	}

	g_pLog->debug("AppId for process %s in %u is %u\n", exe.filename().c_str(), pipeHandle, appId);
	return appId;
}

std::filesystem::path Process_t::getRealExe()
{
	std::error_code error;
	const auto linkTarget = std::filesystem::read_symlink(getPath("exe"), error);
	if (error)
	{
		return {};
	}
	const auto targetName = linkTarget.filename();

	if (targetName != "wine-preloader" && targetName != "wine64-preloader")
	{
		//Native game
		return linkTarget;
	}

	//Wine does not point to the actual .exe files, so we iterate the open
	//files and pick the one ending with .exe
	const auto maps = getPath("map_files");
	std::filesystem::directory_iterator link(maps, error);
	const std::filesystem::directory_iterator end;
	while (!error && link != end)
	{
		std::error_code linkError;
		const auto path =
			std::filesystem::read_symlink(link->path(), linkError).string();

		if (!linkError && path.ends_with(".exe"))
		{
			return path;
		}

		link.increment(error);
	}

	return linkTarget;
}

bool Process_t::init(const pid_t pid, const HSteamPipe pipeHandle)
{
	this->pid = pid;
	this->pipeHandle = pipeHandle;

	exe = getRealExe();
	if (exe.empty())
	{
		return false;
	}

	environ = readFile("environ");

	if (!environ.size())
	{
		return false;
	}

	appId = getAppIdFromEnv();
	if (!appId) //Will fail on steam process
	{
		return false;
	}

	if (!g_config.smartTickets.get())
	{
		return true;
	}

	if (exe.filename().string().ends_with(".exe"))
	{
		file = std::make_unique<CPortableExecutableFile>();

		if (!file->load(exe))
		{
			return false;
		}
	}

	steamDRM = file->hasSteamDRM();
	denuvo = file->hasDenuvo();

	if (steamDRM)
	{
		LOG_DEBUG("Detected SteamDRM in %s!\n", exe.filename().c_str());
	}

	if (denuvo)
	{
		LOG_DEBUG("Detected Denuvo in %s!\n", exe.filename().c_str());
	}

	return true;
}

std::unordered_map<HSteamPipe, Process_t> g_processMap = std::unordered_map<HSteamPipe, Process_t>();
