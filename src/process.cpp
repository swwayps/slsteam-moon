#include "process.hpp"

#include "sdk/CSteamEngine.hpp"

#include "log.hpp"
#include "utils.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>


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
	auto reAppId = std::regex("SteamAppId=[0-9]+");
	std::smatch appIdMatch;

	if (!std::regex_search(environ, appIdMatch, reAppId))
	{
		g_pLog->warn("No SteamAppId in %s's environment! Using 0\n", exe.filename().c_str());
		return 0;
	}

	reAppId = std::regex("[0-9]+");
	const auto envVar = appIdMatch.str();

	std::regex_search(envVar, appIdMatch, reAppId);
	AppId_t appId = std::stoul(appIdMatch.str());

	g_pLog->debug("AppId for process %s in %u is %u\n", exe.filename().c_str(), pipeHandle, appId);
	return appId;
}

std::filesystem::path Process_t::getRealExe()
{
	const auto linkTarget = std::filesystem::read_symlink(getPath("exe"));
	const auto targetName = linkTarget.filename();

	if (targetName != "wine-preloader" && targetName != "wine64-preloader")
	{
		//Native game
		return linkTarget;
	}

	//Wine does not point to the actual .exe files, so we iterate the open
	//files and pick the one ending with .exe
	const auto maps = getPath("map_files");
	for (const auto& link : std::filesystem::directory_iterator { maps })
	{
		const auto path = std::filesystem::read_symlink(link).string();

		if (path.ends_with(".exe"))
		{
			return path;
		}
	}

	return linkTarget;
}

bool Process_t::init(const pid_t pid, const HSteamPipe pipeHandle)
{
	this->pid = pid;
	this->pipeHandle = pipeHandle;

	const auto serverPipe = g_pSteamEngine->getServerPipe(pipeHandle);
	if (!serverPipe)
	{
		g_pLog->warn("ServerPipe for %u is null!\n", pipeHandle);
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

	return true;
}

std::unordered_map<HSteamPipe, Process_t> g_processMap = std::unordered_map<HSteamPipe, Process_t>();
