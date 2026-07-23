#include "filewatcher.hpp"

#include "log.hpp"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	// inotify mask per path.  Files are watched for content writes (config
	// rewrites arrive via atomic rename, re-armed after each event).
	// Directories (e.g. <Steam>/config/stplug-in) must instead watch for
	// entries being created/removed/moved, since IN_MODIFY on a directory
	// does NOT fire when a .lua is dropped in or deleted — that is how games
	// are added/removed in the new-version layout.
	uint32_t maskForPath(const char* path)
	{
		struct stat st{};
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		{
			return IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM
			     | IN_CLOSE_WRITE;
		}
		return IN_MODIFY;
	}
}


//TODO: Investigate why gcc complains when put into CFileWatcher itself
void* watchLoop(void* args)
{
	auto watcher = reinterpret_cast<CFileWatcher*>(args);
	g_pLog->debug("Started FileWatcher %u\n", watcher->notifyFd);

	for(;;)
	{
		g_pLog->debug("Watching for changes...\n");

		// Read a full batch.  Directory events carry a trailing name of
		// variable length, so a fixed sizeof(inotify_event) read would leave
		// the name bytes in the kernel buffer and mis-align the next read.
		// We don't parse individual events (onModify reloads everything), we
		// just need to drain the batch and fire one reload.
		alignas(inotify_event) char buf[4096];
		ssize_t size = read(watcher->notifyFd, buf, sizeof(buf));
		if (size <= 0)
		{
			continue;
		}

		g_pLog->debug("inotify batch bytes=%zd\n", size);
		watcher->onModify();

		// The config is rewritten via atomic rename (write tmp, then rename over
		// the target), which swaps the file's inode. inotify watches the inode,
		// so the original watch is auto-removed (IN_IGNORED) on the first such
		// write and every later change would go unnoticed — the live reload
		// would silently stop after one edit. Re-arm on the current inode after
		// each event so repeated writes (e.g. successive menu pin/unlock saves)
		// keep reloading.
		watcher->rearm();
	}

	return nullptr;
}

CFileWatcher::CFileWatcher(FileModifyEvent_t onModify)
{
	this->onModify = onModify;

	notifyFd = inotify_init();
	g_pLog->debug("Created notify fd %i\n", notifyFd);
}

CFileWatcher::~CFileWatcher()
{
	if (watchThread)
	{
		stop();
	}

	// Closing the inotify instance removes all of its watches; the per-watch
	// descriptors are watch ids (not file descriptors) and must not be close()d.
	if (notifyFd != -1)
	{
		close(notifyFd);
	}
}

bool CFileWatcher::addFile(const char* path)
{
	int fd = inotify_add_watch(notifyFd, path, maskForPath(path));
	if (fd == -1)
	{
		return false;
	}

	watchedPaths.emplace_back(path);
	fileFdMap[fd] = path;
	g_pLog->debug("Added %s to FileWatcher %i\n", path, notifyFd);
	return true;
}

// Re-add the inotify watch for every tracked path. inotify_add_watch is
// idempotent for an unchanged inode (it returns the existing watch id), and
// attaches to the NEW inode after an atomic-rename replace — so this both
// refreshes a live watch and recovers one the kernel dropped via IN_IGNORED.
void CFileWatcher::rearm()
{
	for (const auto& path : watchedPaths)
	{
		int fd = inotify_add_watch(notifyFd, path.c_str(), maskForPath(path.c_str()));
		if (fd != -1)
		{
			fileFdMap[fd] = path;
		}
	}
}

bool CFileWatcher::start()
{
	int code = pthread_create(&watchThread, nullptr, &watchLoop, this);
	return code == 0;
}

void CFileWatcher::stop()
{
	pthread_cancel(watchThread);
}
