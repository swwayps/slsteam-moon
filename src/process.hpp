#pragma once

#include "sdk/steam.hpp"

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <unordered_map>


struct Process_t
{
	pid_t pid;
	std::filesystem::path exe;
	std::string environ;

	AppId_t appId;
	HSteamPipe pipeHandle;

	std::filesystem::path getPath(const char* fileName);
	std::string readFile(const char* fileName);

	AppId_t getAppIdFromEnv();
	std::filesystem::path getRealExe();

	bool init(const pid_t pid, const HSteamPipe pipeHandle);
};

extern std::unordered_map<HSteamPipe, Process_t> g_processMap;
