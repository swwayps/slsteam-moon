#pragma once

#include <cstddef>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>


typedef void(*FileModifyEvent_t)();

class CFileWatcher
{
	pthread_t watchThread;

public:
	struct Watch
	{
		std::string path;
		// For a directory: the entry names whose events count. Empty: all.
		std::vector<std::string> names;
	};

	int notifyFd;
	// Watch id -> index in watchedPaths.
	std::unordered_map<int, std::size_t> fileFdMap;
	// Every watched path, so the watch can be re-armed after an atomic-rename
	// replace swaps the file's inode (see rearm()).
	std::vector<Watch> watchedPaths;

	FileModifyEvent_t onModify;

	CFileWatcher(const FileModifyEvent_t onModify);
	~CFileWatcher();

	bool addFile(const char* path, std::vector<std::string> names = {});
	// Whether a batch of inotify events holds one that should reload.
	bool wants(const char* events, std::size_t size) const;
	void rearm();
	bool start();
	void stop();

	// Cooperatively stop and join every running watcher thread. Registered with
	// std::atexit on the first start(), so it runs before the static
	// destructors free g_pLog/g_config — the leaked, unjoined watcher thread
	// must not touch those globals after teardown (a use-after-free that
	// surfaced as a logger crash under LogLevel 1). Idempotent.
	static void stopAllForShutdown();
};
