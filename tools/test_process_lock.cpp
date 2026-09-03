// Regression test for the process-wide file lock used by the audit setup.

#include "../src/utils/process_lock.hpp"

#include <cassert>
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

		// A second audited namespace opens the same inode independently.  It
		// must observe the first namespace's lock instead of running setup too.
		ProcessLock::FileLock second(path);
		assert(!second.acquired());
	}

	ProcessLock::FileLock afterRelease(path);
	assert(afterRelease.acquired());

	std::filesystem::remove(path, ec);
	return 0;
}
