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

bool IExecutableFile::hasSteamDRM()
{
	if (!(g_config.smartTickets.get() & CConfig::k_ESmartTicketsSteamDRM))
	{
		return false;
	}
	//SteamDRM appends .bind section with very high entropy (usually 7.9+)

	//A validly-parsed but section-stripped/packed binary (common for DRM'd
	//titles) can leave this empty. size() - 1 would underflow and .at() would
	//throw std::out_of_range, aborting the whole Steam client from the
	//unguarded ConnectPipe analysis path.
	if (sections.empty())
	{
		return false;
	}

	const auto& last = sections.back();

	if (last.name == ".bind")
	{
		const auto bytes = readSection(last);
		const double entropy = Utils::calculateEntropy(bytes);

		if (g_pLog) g_pLog->debug("%s has entropy of %f\n", last.name.c_str(), entropy);

		if (entropy >= 7.0)
		{
			return true;
		}
	}

	return false;
}

bool IExecutableFile::hasDenuvo()
{
	if (!(g_config.smartTickets.get() & CConfig::k_ESmartTicketsDenuvo))
	{
		return false;
	}

	static const std::vector<std::string> X_SECS =
	{
		".xtext",
		".xcode",
		//".xdata", //Causes a lot of false positives
		".xpdata",
		".xtls"
	};

	static const std::vector<std::string> CODE_SECS =
	{
		//Start with potentially smaller sections first
		".text",
		".code",

		".xcode",
		".xtext",

		".text1",
	};

	constexpr double MIN_ENTROPY = 6.25;

	unsigned secsFound = 0;
	double codeEntropy = 0.0;

	for (const auto& sec : sections)
	{
		for (const auto& xsec : X_SECS)
		{
			if (sec.name == xsec)
			{
				secsFound++;
				break;
			}
		}

		//Do not waste time reading sections when MIN is already met
		if (codeEntropy >= MIN_ENTROPY)
		{
			continue;
		}

		for (const auto& csec : CODE_SECS)
		{
			if (sec.name != csec)
			{
				continue;
			}

			const auto bytes = readSection(sec);
			const double entropy = Utils::calculateEntropy(bytes);

			if (g_config.extendedLogging.get())
			{
				if (g_pLog) g_pLog->debug("%s entropy is %f\n", sec.name.c_str(), entropy);
			}

			codeEntropy = std::max(codeEntropy, entropy);
		}
	}

	if (g_pLog) g_pLog->debug("%u Denuvo sections, entropy %f\n", secsFound, codeEntropy);

	//At least one denuvo section & and code has to encrypted/obfuscated
	return secsFound > 0 && codeEntropy > MIN_ENTROPY;
}

std::vector<uint8_t> IExecutableFile::readSection(const SectionHdr_t& section)
{
	//offset/size come straight from on-disk headers. A corrupt or hostile
	//binary can advertise a multi-gigabyte section; resize() would then throw
	//bad_alloc/length_error (unreliable to catch across the release build's
	//.cold EH partition) or fread would block on a huge read. Reject anything
	//that does not fit inside the file.
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
			logFailure("Failed to read section header %i!\n", i);
			return false;
		}

		char name[SECTION_HEADER_NAME_SIZE];
		strncpy(name, reinterpret_cast<char*>(sectHdr), sizeof(name));

		const uint32_t rva = *reinterpret_cast<uint32_t*>(&sectHdr[0xC]);
		const uint32_t size = *reinterpret_cast<uint32_t*>(&sectHdr[0x10]);
		const uint32_t ptr = *reinterpret_cast<uint32_t*>(&sectHdr[0x14]);

		if (g_config.extendedLogging.get())
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

		if (g_config.extendedLogging.get())
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

		if (g_config.extendedLogging.get())
		{
			if (g_pLog) g_pLog->debug("Section header name %s, address 0x%llx, offset 0x%llx\n", name, shdr.sh_addr, shdr.sh_offset);
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

	if (hdr64.e_ident[EI_CLASS] == ELFCLASS32 && hdr64.e_machine == ISA_X86)
	{
		if (g_pLog) g_pLog->debug("Parsing as 32 bit file\n");

		//Headers are the same till e_entry. The 64bit version is longer
		//so we can just recast it
		Elf32_Ehdr hdr32 = *reinterpret_cast<Elf32_Ehdr*>(&hdr64);
		parseElf32Headers(hdr32);
	}
	else if (hdr64.e_ident[EI_CLASS] == ELFCLASS64 && hdr64.e_machine == ISA_AMD64)
	{
		if (g_pLog) g_pLog->debug("Parsing as 64 bit file\n");
		parseElf64Headers(hdr64);
	}
	else
	{
		logFailure("Unknown ELFCLASS/e_machine %u | %u!\n", hdr64.e_ident[EI_CLASS], hdr64.e_machine);
		return false;
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

std::unordered_set<std::filesystem::path> Process_t::getOpenFiles()
{
	//Using a set because we do not want duplicates
	auto files = std::unordered_set<std::filesystem::path>();

	const auto maps = getPath("map_files");
	std::error_code error;
	std::filesystem::directory_iterator file(maps, error);
	const std::filesystem::directory_iterator end;
	while (!error && file != end)
	{
		//Afaik all files should be symlinks. But better safe than sorry
		std::error_code linkError;
		if (std::filesystem::is_symlink(file->path(), linkError) && !linkError)
		{
			const auto path = std::filesystem::read_symlink(file->path(), linkError);
			if (!linkError)
			{
				files.emplace(path);
			}
		}
		else
		{
			files.emplace(file->path());
		}

		file.increment(error);
	}

	return files;
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
	const auto files = getOpenFiles();
	for (const auto& path : files)
	{
		if (path.string().ends_with(".exe"))
		{
			return path;
		}
	}

	return linkTarget;
}

bool Process_t::analyse()
{
	const auto startTime = std::chrono::system_clock::now();
	const auto exeFile = IExecutableFile::create(exe);

	if (!exeFile)
	{
		if (g_pLog) g_pLog->warn("Failed to parse %s!\n", exe.filename().c_str());
		return false;
	}

	steamDRM = exeFile->hasSteamDRM();
	denuvo = exeFile->hasDenuvo();

	//The mapped-file scan below exists only to catch Denuvo hidden in a game's
	//dynamic link libraries. SteamStub's high-entropy ".bind" section is only
	//ever appended to a title's main executable, never to the shared libraries
	//it maps, so scanning them for SteamDRM finds nothing the exe parse missed.
	//When Denuvo detection is off (the shipped default, SmartTickets=0x1) the
	//scan therefore does hundreds of full PE/ELF parses on the Steam engine
	//thread for no result, which trips Steam's >15s BMainLoop watchdog and
	//fatally exits the client. Only walk the mapped files when Denuvo detection
	//is enabled and still unresolved.
	//Trade-off: a Proton title whose real .exe is not the one getRealExe()
	//resolved (a launcher shim) no longer has its SteamDRM detected via the
	//library scan under the default config; that already relied on a
	//nondeterministic exe pick. Enable the Denuvo bit to restore the full scan.
	if ((g_config.smartTickets.get() & CConfig::k_ESmartTicketsDenuvo) && !denuvo)
	{
		for (const auto& file : getOpenFiles())
		{
			const auto executable = IExecutableFile::create(file, LogLevel::Debug);
			if (!executable)
			{
				continue;
			}

			if (!steamDRM)
			{
				steamDRM |= executable->hasSteamDRM();
			}

			if (!denuvo)
			{
				denuvo |= executable->hasDenuvo();
			}
		}
	}

	if (steamDRM)
	{
		if (g_pLog) g_pLog->debug("Detected SteamDRM in %s!\n", exe.filename().c_str());
	}

	if (denuvo)
	{
		if (g_pLog) g_pLog->debug("Detected Denuvo in %s!\n", exe.filename().c_str());
	}

	const auto endTime = std::chrono::system_clock::now();
	if (g_pLog) g_pLog->debug("Analysed %s in %llums\n", exe.filename().c_str(), std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());
	return true;
}

bool Process_t::init(const pid_t pid, const HSteamPipe pipeHandle)
{
	this->pid = pid;
	this->pipeHandle = pipeHandle;

	const auto serverPipe = g_pSteamEngine->getServerPipe(pipeHandle);
	if (!serverPipe)
	{
		if (g_pLog) g_pLog->warn("ServerPipe for %p is null!\n", reinterpret_cast<void*>(pipeHandle));
		return false;
	}

	exe = getRealExe();
	if (!exe.string().size())
	{
		return false;
	}

	cmdLine = Utils::strsplit(const_cast<char*>(readFile("cmdline").c_str()), "\0");
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

	analyse();

	return true;
}

std::unordered_map<HSteamPipe, Process_t> g_processMap = std::unordered_map<HSteamPipe, Process_t>();
