#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>


class CFileWatcher;

namespace SLSAPI
{
	// Resolved at init() inside the user's private runtime directory; empty (and
	// `path` an empty string) until then.
	extern std::string contractPath;
	struct InstallCommand_t
	{
		uint32_t appId;
		uint32_t libraryIndex;
	};

	extern const char* path;
	extern std::fstream fstream;
	extern CFileWatcher* watcher;

	extern std::mutex executionMutex;
	extern std::vector<InstallCommand_t> installs;
	extern std::vector<uint32_t> uninstalls;

	bool isEnabled();
	void onFileChange();
	void init();

	void runIPCFrame();
}
