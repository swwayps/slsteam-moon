#pragma once
// Private per-user location for SLSsteam's runtime contracts (the API command
// file, the one-shot load lock).
//
// Both used to live in /tmp under a fully predictable name:
//   /tmp/SLSsteam.API          — a control channel with no access control at
//                                all. Any local process that can write to it
//                                submits `install|<appid>|<library>`.
//   /tmp/.slssteam.load.<pid>  — the "only hook once per process" lock, opened
//                                without O_EXCL or O_NOFOLLOW. A process that
//                                pre-creates it and holds an exclusive flock
//                                makes our flock(LOCK_NB) fail, and the old code
//                                then SKIPPED the hooking pass — an injection
//                                kill switch available to any local process.
//
// $XDG_RUNTIME_DIR is per-user and mode 0700, so a directory under it is not
// reachable by other users at all. Everything here is header-only and pure apart
// from the explicitly named filesystem calls, so it can be unit tested.
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace RuntimeDir
{
	// Relative directory used under whichever base is chosen.
	inline const char* kSubdir = "SLSsteam";

	// resolveBase(xdgRuntimeDir, home) -> the directory that should hold our
	// private files, or "" when neither input is usable.
	//
	// $XDG_RUNTIME_DIR is preferred. Its absence (a bare session, a container)
	// falls back to a directory under $HOME — never /tmp, which is shared with
	// every other local user and is the whole reason these files were reachable.
	inline std::string resolveBase(const char* xdgRuntimeDir, const char* home)
	{
		if (xdgRuntimeDir != nullptr && xdgRuntimeDir[0] == '/')
		{
			return std::string(xdgRuntimeDir) + "/" + kSubdir;
		}
		if (home != nullptr && home[0] == '/')
		{
			return std::string(home) + "/.cache/" + kSubdir;
		}
		return std::string();
	}

	// isPrivateDir(mode, uid, euid) -> true when a directory with this stat data
	// is one we own and nobody else can enter or write.
	inline bool isPrivateDir(mode_t mode, uid_t uid, uid_t euid)
	{
		if (!S_ISDIR(mode))
		{
			return false;
		}
		if (uid != euid)
		{
			return false;
		}
		return (mode & (S_IRWXG | S_IRWXO)) == 0;
	}

	// isPrivateFile(mode, uid, euid) -> true when a regular file with this stat
	// data is one we own and nobody else can read or write.
	inline bool isPrivateFile(mode_t mode, uid_t uid, uid_t euid)
	{
		if (!S_ISREG(mode))
		{
			return false;
		}
		if (uid != euid)
		{
			return false;
		}
		return (mode & (S_IRWXG | S_IRWXO)) == 0;
	}

	// ensureDir(path) -> true when `path` is, after this call, a private
	// directory we own. An existing directory that is a symlink, owned by someone
	// else, or accessible to group/others is REFUSED rather than reused: anything
	// already inside it may not be ours.
	inline bool ensureDir(const std::string& path)
	{
		if (path.empty())
		{
			return false;
		}
		struct stat st{};
		if (lstat(path.c_str(), &st) == 0)
		{
			return isPrivateDir(st.st_mode, st.st_uid, geteuid());
		}
		if (errno != ENOENT)
		{
			return false;
		}
		// Create the parent chain best-effort (only ~/.cache needs it; a
		// $XDG_RUNTIME_DIR always exists), then the private directory itself.
		const std::size_t slash = path.rfind('/');
		if (slash != std::string::npos && slash > 0)
		{
			mkdir(path.substr(0, slash).c_str(), 0700);
		}
		if (mkdir(path.c_str(), 0700) != 0)
		{
			return false;
		}
		// mkdir intersects the requested mode with the umask, so ask again.
		if (chmod(path.c_str(), 0700) != 0)
		{
			return false;
		}
		struct stat verify{};
		if (lstat(path.c_str(), &verify) != 0)
		{
			return false;
		}
		return isPrivateDir(verify.st_mode, verify.st_uid, geteuid());
	}

	// descriptorIsPrivate(fd) -> true when the open descriptor refers to a
	// regular file we own that nobody else can read or write.
	inline bool descriptorIsPrivate(int fd)
	{
		if (fd < 0)
		{
			return false;
		}
		struct stat st{};
		if (fstat(fd, &st) != 0)
		{
			return false;
		}
		return isPrivateFile(st.st_mode, st.st_uid, geteuid());
	}
}
