#include "process.hpp"

#include "log.hpp"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>


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

	return true;
}

std::unordered_map<HSteamPipe, Process_t> g_processMap = std::unordered_map<HSteamPipe, Process_t>();
