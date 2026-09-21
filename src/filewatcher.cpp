#include "filewatcher.hpp"

#include "filewatcher_burst.hpp"

#include "log.hpp"
#include "ownerwork.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <poll.h>
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

	// Cooperative shutdown for the watcher thread(s).  The thread is a leaked,
	// unjoined daemon: at process exit the static destructors free the globals
	// it uses (g_pLog's ofstream, g_config, ...), and if the thread is still
	// looping it dereferences freed memory.  It surfaced as a crash inside the
	// logger's std::ostream on LogLevel 1, where the loop logs on every
	// iteration and so is almost always mid-write when teardown begins.
	//
	// stopAllForShutdown() is registered with std::atexit from start(). Because
	// it is registered at runtime — after those globals finished construction —
	// the standard sequences it BEFORE their destructors, so it joins the
	// thread while g_pLog/g_config are still valid.
	std::atomic<bool>       g_watcherStopping{false};
	std::mutex              g_watcherThreadsMu;
	std::vector<pthread_t>  g_watcherThreads;
	std::once_flag          g_watcherAtexitOnce;
}


//TODO: Investigate why gcc complains when put into CFileWatcher itself
void* watchLoop(void* args)
{
	auto watcher = reinterpret_cast<CFileWatcher*>(args);
	g_pLog->debug("Started FileWatcher %u\n", watcher->notifyFd);

	for(;;)
	{
		// Bail before touching any global (g_pLog below first) once shutdown
		// has been requested, so the thread never races the static destructors
		// that free them. stopAllForShutdown() joins us, so g_pLog stays valid
		// until we return here.
		if (g_watcherStopping.load(std::memory_order_relaxed))
		{
			return nullptr;
		}

		g_pLog->debug("Watching for changes...\n");

		// Bounded wait instead of a blocking read, so this thread also gets a
		// periodic tick with NO extra thread (spawning threads from the
		// LD_AUDIT preinit path is a recorded anti-pattern). The tick drives
		// the owner-queue staleness sweep: watcher-originated Steam work is
		// handed to the owner IPC thread without waiting, and if the owner
		// never drains it, this tick eventually runs it here instead of losing
		// it (see ownerwork.hpp). Cost is 2 idle wakeups per second, each a
		// single relaxed atomic load when nothing is pending.
		struct pollfd pfd{};
		pfd.fd = watcher->notifyFd;
		pfd.events = POLLIN;
		const int ready = poll(&pfd, 1, OwnerWork::kIdleTickMs);
		if (ready <= 0 || (pfd.revents & POLLIN) == 0)
		{
			// Timeout, EINTR, or a non-readable event: no inotify batch to
			// handle, just service the queue and go round again.
			OwnerWork::idleTick();
			continue;
		}

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
		size += static_cast<ssize_t>(
			FileWatcherBurst::drainAdditionalReadable(watcher->notifyFd));

		// A shutdown request can arrive while poll()/read() were blocked; don't
		// start a reload (which logs and touches g_config) if we're tearing
		// down.
		if (g_watcherStopping.load(std::memory_order_relaxed))
		{
			return nullptr;
		}

		g_pLog->debug("inotify batch bytes=%zd\n", size);

		// The config is rewritten via atomic rename (write tmp, then rename over
		// the target), which swaps the file's inode. inotify watches the inode,
		// so the original watch is auto-removed (IN_IGNORED) on the first such
		// write and every later change would go unnoticed — the live reload
		// would silently stop after one edit. Re-arm on the current inode after
		// each event so repeated writes (e.g. successive menu pin/unlock saves)
		// keep reloading.
		//
		// Re-arm BEFORE running the callback, not after: anything written
		// during an unarmed window is lost outright, and the callback does
		// non-trivial local work (config reload, Lua import) even though it no
		// longer blocks on Steam. Re-arming first shrinks that window to
		// nothing — writes that land while the callback runs simply queue in
		// the kernel and are read on the next iteration. inotify_add_watch is
		// idempotent for an unchanged inode, so this is the same work, just
		// earlier.
		watcher->rearm();

		watcher->onModify();
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
	if (code != 0)
	{
		return false;
	}

	// Track the thread so it can be joined at process exit, and register the
	// shutdown hook exactly once. Registered here (at runtime) so std::atexit
	// sequences it before the static destructors that free g_pLog/g_config.
	{
		std::lock_guard<std::mutex> lock(g_watcherThreadsMu);
		g_watcherThreads.push_back(watchThread);
	}
	std::call_once(g_watcherAtexitOnce, []
	{
		std::atexit(&CFileWatcher::stopAllForShutdown);
	});
	return true;
}

void CFileWatcher::stop()
{
	pthread_cancel(watchThread);
}

void CFileWatcher::stopAllForShutdown()
{
	g_watcherStopping.store(true, std::memory_order_relaxed);

	std::vector<pthread_t> threads;
	{
		std::lock_guard<std::mutex> lock(g_watcherThreadsMu);
		threads.swap(g_watcherThreads);
	}

	// Join outside the lock: the thread wakes from its bounded poll() within
	// kIdleTickMs, sees the flag, and returns. Joining guarantees it is gone
	// before we hand control back to the exit sequence that frees the globals.
	for (const pthread_t thread : threads)
	{
		pthread_join(thread, nullptr);
	}
}
