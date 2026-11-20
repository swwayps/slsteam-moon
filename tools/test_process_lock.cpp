// Regression test for the process-wide file lock used by the audit setup.

#include "../src/utils/process_lock.hpp"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// Linked with -Wl,--wrap=flock: a non-zero value makes flock fail with it.
static int g_flockError = 0;
extern "C" int __real_flock(int fd, int operation);
extern "C" int __wrap_flock(int fd, int operation)
{
	if (g_flockError != 0)
	{
		errno = g_flockError;
		return -1;
	}
	return __real_flock(fd, operation);
}

static void onAlarm(int) {}

int main()
{
	std::error_code ec;
	const std::string path =
		"/tmp/slssteam-test-process-lock." + std::to_string(getpid());
	std::filesystem::remove(path, ec);

	{
		ProcessLock::FileLock first(path);
		assert(first.acquired());
		assert(!first.heldByAnother());

		// A second audited namespace opens the same inode independently.  It
		// must observe the first namespace's lock instead of running setup too.
		ProcessLock::FileLock second(path);
		assert(!second.acquired());
		// Real contention: a trustworthy lock exists and someone else holds it,
		// so the guarded work must be skipped.
		assert(second.usable());
		assert(second.heldByAnother());
	}

	ProcessLock::FileLock afterRelease(path);
	assert(afterRelease.acquired());
	assert(!afterRelease.heldByAnother());
	std::filesystem::remove(path, ec);

	// No lock we can trust: the path is group/other-accessible, so it is not a
	// private file of ours and carries no ownership information. The caller MUST
	// proceed -- treating this as contention is what turned an unusable lock
	// path into a silent off switch for the work it guards.
	{
		const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
		assert(fd >= 0);
		::close(fd);
		ProcessLock::FileLock shared(path);
		assert(!shared.acquired());
		assert(!shared.usable());
		assert(!shared.heldByAnother());
		std::filesystem::remove(path, ec);
	}

	// Same conclusion when the lock cannot be opened at all (missing parent).
	{
		ProcessLock::FileLock missingDir(
			"/tmp/slssteam-test-process-lock-absent-dir." +
			std::to_string(getpid()) + "/lock");
		assert(!missingDir.acquired());
		assert(!missingDir.usable());
		assert(!missingDir.heldByAnother());
	}

	// A blocking wait interrupted by a signal (a handler without SA_RESTART)
	// keeps waiting: it is not contention, and reading it as "held elsewhere"
	// skipped the guarded work for that pass.
	{
		int ready[2];
		assert(pipe(ready) == 0);
		const pid_t holder = fork();
		if (holder == 0)
		{
			ProcessLock::FileLock held(path);
			const char byte = held.acquired() ? 1 : 0;
			(void)!write(ready[1], &byte, 1);
			sleep(2);
			_exit(0);
		}
		char byte = 0;
		assert(read(ready[0], &byte, 1) == 1 && byte == 1);
		struct sigaction action {};
		action.sa_handler = onAlarm;
		action.sa_flags = 0;
		sigemptyset(&action.sa_mask);
		sigaction(SIGALRM, &action, nullptr);
		alarm(1);
		ProcessLock::FileLock waited(path, false);
		const bool acquired = waited.acquired();
		const bool held = waited.heldByAnother();
		waitpid(holder, nullptr, 0);
		if (!acquired) std::puts("FAIL: an interrupted blocking wait gave up on the lock");
		assert(acquired && !held);
	}
	std::filesystem::remove(path, ec);

	// Any flock error other than contention (ENOLCK on a network home) is no
	// lock we can trust, so the caller proceeds.
	{
		g_flockError = ENOLCK;
		ProcessLock::FileLock broken(path, false);
		g_flockError = 0;
		if (broken.heldByAnother()) std::puts("FAIL: an flock error reads as held by another");
		assert(!broken.acquired());
		assert(!broken.usable());
		assert(!broken.heldByAnother());
	}
	std::filesystem::remove(path, ec);

	return 0;
}
