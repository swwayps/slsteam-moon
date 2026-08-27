// SPDX-License-Identifier: AGPL-3.0-only
//
// Small advisory-lock primitive shared by the audit setup and the on-disk
// cache writers.  flock() is deliberately used instead of a pid/sentinel
// protocol: the kernel releases it when the owning namespace/process dies.

#pragma once

#include "../runtimedir.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

namespace ProcessLock
{

class FileLock
{
public:
	explicit FileLock(const std::string& path, bool nonBlocking = true)
		: path_(path)
	{
		if (path.empty()) return;

		// O_NOFOLLOW: a symlink planted at the lock path must not redirect the
		// open onto something else.
		fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd_ < 0) return;

		// Only a private regular file we own is a lock we can reason about.
		// Anything else counts as "no lock here", which callers must treat as
		// "proceed" — otherwise a planted file switches the guarded work off.
		if (!RuntimeDir::descriptorIsPrivate(fd_))
		{
			::close(fd_);
			fd_ = -1;
			return;
		}

		usable_ = true;

		const int mode = LOCK_EX | (nonBlocking ? LOCK_NB : 0);
		if (::flock(fd_, mode) != 0)
		{
			::close(fd_);
			fd_ = -1;
		}
	}

	~FileLock()
	{
		if (fd_ >= 0) ::close(fd_);
	}

	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;

	FileLock(FileLock&& other) noexcept
		: path_(std::move(other.path_)), fd_(other.fd_), usable_(other.usable_)
	{
		other.fd_ = -1;
		other.usable_ = false;
	}

	FileLock& operator=(FileLock&& other) noexcept
	{
		if (this == &other) return *this;
		if (fd_ >= 0) ::close(fd_);
		path_ = std::move(other.path_);
		fd_ = other.fd_;
		usable_ = other.usable_;
		other.fd_ = -1;
		other.usable_ = false;
		return *this;
	}

	bool acquired() const { return fd_ >= 0; }
	// usable() separates the two reasons acquired() can be false:
	//   usable() true  -> the lock file is ours and somebody else holds it, so
	//                     the caller SHOULD skip the guarded work.
	//   usable() false -> there is no lock here we can trust (no private
	//                     directory, open refused, or the file is not a private
	//                     regular file of ours), so the caller MUST proceed.
	// Skipping in the second case is what turned a lock file into an off switch.
	bool usable() const { return usable_; }
	int fd() const { return fd_; }
	const std::string& path() const { return path_; }

private:
	std::string path_;
	int fd_ = -1;
	bool usable_ = false;
};

// Per-process lock path inside the user's PRIVATE runtime directory.
//
// This used to be "/tmp/<prefix>.<pid>", which any local process could
// pre-create and hold an exclusive flock on. Since a caller that fails to
// acquire treats it as "somebody else is doing this work" and skips that work,
// a lock in a shared directory is a remote off switch for whatever it guards.
// Returns "" when no private directory can be prepared; callers must treat that
// as "no lock available" and PROCEED rather than skip.
inline std::string perProcessPath(const char* prefix, pid_t pid = ::getpid())
{
	const std::string dir = RuntimeDir::resolveBase(::getenv("XDG_RUNTIME_DIR"),
	                                                ::getenv("HOME"));
	if (dir.empty() || !RuntimeDir::ensureDir(dir)) return std::string();
	char name[128];
	std::snprintf(name, sizeof(name), "/%s.%d", prefix,
	              static_cast<int>(pid));
	return dir + name;
}

} // namespace ProcessLock
