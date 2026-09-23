// Regression test for the process-wide file lock used by the audit setup.

#include "../src/utils/process_lock.hpp"

#include <cassert>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>

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

	return 0;
}
