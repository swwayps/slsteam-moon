#pragma once

#include "sdk/sdk.hpp"

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

	struct CompatOp_t
	{
		enum class OpType
		{
			Get,
			Set
		};

		OpType type;
		AppId_t appId;
		std::string tool;
	};

	struct LibraryOp_t
	{
		enum class OpType
		{
			Dump,
			Install,
			Uninstall
		};

		OpType type;
		AppId_t appId;
		uint32_t libraryIndex;
	};

	extern const char* path;
	extern std::fstream fstream;
	extern CFileWatcher* watcher;

	extern std::mutex cmdMutex;
	extern std::vector<CompatOp_t> compatOps;
	extern std::vector<LibraryOp_t> libraryOps;

	bool isEnabled();
	void onFileChange();
	void init();
	void parseCmd(const std::string& cmd);
	void runCompatOps();
	void runInstallOps();
	void runIPCFrame();
}
