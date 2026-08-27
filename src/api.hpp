#pragma once

#include <fstream>
#include <string>


class CFileWatcher;

namespace SLSAPI
{
	// Resolved at init() inside the user's private runtime directory; empty (and
	// `path` an empty string) until then.
	extern std::string contractPath;
	extern const char* path;
	extern std::fstream fstream;
	extern CFileWatcher* watcher;

	bool isEnabled();
	void onFileChange();
	void init();
}
