// SPDX-License-Identifier: AGPL-3.0-only
//
// Write a complete file to a private temporary inode, fsync it, and publish it
// with rename(). Readers therefore see either the previous complete record or
// the new complete record; never a truncated buffer or half-written YAML.
// Inside a DeferSync scope the fsyncs wait for the end of the scope.

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#ifdef ATOMIC_FILE_TESTING
#include <functional>
#endif

namespace AtomicFile
{

struct FileIdentity
{
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;
	std::int64_t mtimeSecs = 0;
	std::int64_t mtimeNsecs = 0;

	bool operator==(const FileIdentity& other) const noexcept
	{
		return device == other.device && inode == other.inode &&
		       size == other.size && mtimeSecs == other.mtimeSecs &&
		       mtimeNsecs == other.mtimeNsecs;
	}
};

inline bool readIdentity(const std::string& path, FileIdentity& out)
{
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0) return false;
	out.device = static_cast<std::uint64_t>(st.st_dev);
	out.inode = static_cast<std::uint64_t>(st.st_ino);
	out.size = static_cast<std::uint64_t>(st.st_size);
	out.mtimeSecs = static_cast<std::int64_t>(st.st_mtime);
	out.mtimeNsecs = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
	return true;
}

namespace detail
{

inline std::atomic<unsigned long> sequence{0};

// The folders the DeferSync scope open on this thread flushes; null outside one.
inline thread_local std::vector<std::string>* deferredFolders = nullptr;

inline bool writeAll(int fd, const char* data, std::size_t size)
{
	std::size_t written = 0;
	while (written < size)
	{
		const ssize_t n = ::write(fd, data + written, size - written);
		if (n > 0)
		{
			written += static_cast<std::size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		return false;
	}
	return true;
}

inline void syncParent(const std::string& path)
{
	const auto parent = std::filesystem::path(path).parent_path();
	const std::string parentPath = parent.empty() ? "." : parent.string();
	if (deferredFolders != nullptr)
	{
		if (std::find(deferredFolders->begin(), deferredFolders->end(), parentPath) ==
		    deferredFolders->end())
			deferredFolders->push_back(parentPath);
		return;
	}
	const int fd = ::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return;
	(void)::fsync(fd);
	::close(fd);
}

inline bool renameExchange(const std::string& first,
                           const std::string& second)
{
#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
	return ::syscall(SYS_renameat2, AT_FDCWD, first.c_str(),
	                 AT_FDCWD, second.c_str(), RENAME_EXCHANGE) == 0;
#else
	(void)first;
	(void)second;
	errno = ENOTSUP;
	return false;
#endif
}

inline bool renameNoReplace(const std::string& first,
                            const std::string& second)
{
#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
	return ::syscall(SYS_renameat2, AT_FDCWD, first.c_str(),
	                 AT_FDCWD, second.c_str(), RENAME_NOREPLACE) == 0;
#else
	if (::link(first.c_str(), second.c_str()) != 0) return false;
	// Keeping both hard links is still fail-closed if unlinking the source
	// races with another actor; the inode remains preserved either way.
	(void)::unlink(first.c_str());
	return true;
#endif
}

inline bool preserveConflict(const std::string& source,
                             const std::string& targetBase)
{
	for (int attempt = 0; attempt < 16; ++attempt)
	{
		const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
		const std::string conflictPath = targetBase +
			".slssteam-conditional-conflict." +
			std::to_string(static_cast<long long>(::getpid())) + "." +
			std::to_string(serial);
		if (renameNoReplace(source, conflictPath))
		{
			syncParent(targetBase);
			return true;
		}
		if (errno != EEXIST) return false;
	}
	errno = EEXIST;
	return false;
}

inline bool prepareTemp(const std::string& path,
                        const char* data,
                        std::size_t size,
                        std::string& tempPath,
                        std::string& error)
{
	tempPath.clear();
	if (path.empty())
	{
		error = "empty path";
		return false;
	}

	mode_t mode = 0644;
	struct stat existing{};
	if (::stat(path.c_str(), &existing) == 0)
		mode = existing.st_mode & 07777;

	const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
	tempPath = path + ".tmp." + std::to_string(
		static_cast<long long>(::getpid())) + "." + std::to_string(serial);
	const int fd = ::open(tempPath.c_str(),
	                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
	if (fd < 0)
	{
		error = "open temporary file failed: " +
		         std::string(std::strerror(errno));
		tempPath.clear();
		return false;
	}

	bool ok = writeAll(fd, data, size);
	if (ok && ::fchmod(fd, mode) != 0) ok = false;
	if (ok && deferredFolders == nullptr && ::fsync(fd) != 0) ok = false;
	const int closeResult = ::close(fd);
	if (closeResult != 0) ok = false;

	if (!ok)
	{
		error = "write/fsync temporary file failed";
		::unlink(tempPath.c_str());
		tempPath.clear();
		return false;
	}
	return true;
}

#ifdef ATOMIC_FILE_TESTING
inline std::function<void()>& beforeConditionalPublishHook()
{
	static std::function<void()> hook;
	return hook;
}

inline std::function<void()>& beforeConditionalRollbackHook()
{
	static std::function<void()> hook;
	return hook;
}
#endif

} // namespace detail

#ifdef ATOMIC_FILE_TESTING
inline void setBeforeConditionalPublishHook(std::function<void()> hook)
{
	detail::beforeConditionalPublishHook() = std::move(hook);
}

inline void setBeforeConditionalRollbackHook(std::function<void()> hook)
{
	detail::beforeConditionalRollbackHook() = std::move(hook);
}
#endif

// While one is open on this thread, writes skip their fsyncs, and the scope
// flushes the file systems they touched with one syncfs when it closes. For
// rebuildable caches written in bulk: rename still makes each write atomic,
// and only durability across a power loss waits for the end of the scope.
class DeferSync
{
public:
	DeferSync() noexcept
	{
		if (detail::deferredFolders == nullptr) detail::deferredFolders = &folders_;
	}
	~DeferSync()
	{
		if (detail::deferredFolders != &folders_) return; // nested in another scope
		detail::deferredFolders = nullptr;
		for (const std::string& folder : folders_)
		{
			const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (fd < 0) continue;
			(void)::syncfs(fd);
			::close(fd);
		}
	}
	DeferSync(const DeferSync&) = delete;
	DeferSync& operator=(const DeferSync&) = delete;

private:
	std::vector<std::string> folders_;
};

inline bool write(const std::string& path, const char* data, std::size_t size,
                 std::string& error)
{
	error.clear();
	std::string tempPath;
	if (!detail::prepareTemp(path, data, size, tempPath, error))
		return false;

	if (::rename(tempPath.c_str(), path.c_str()) != 0)
	{
		error = "rename failed: " + std::string(std::strerror(errno));
		::unlink(tempPath.c_str());
		return false;
	}
	detail::syncParent(path);
	return true;
}

inline bool writeIfUnchanged(const std::string& path,
                             const FileIdentity& expected,
                             const char* data, std::size_t size,
                             std::string& error)
{
	error.clear();
	FileIdentity current{};
	if (!readIdentity(path, current))
	{
		error = "file disappeared before publish";
		return false;
	}
	if (!(current == expected))
	{
		error = "file changed before publish";
		return false;
	}

	std::string tempPath;
	if (!detail::prepareTemp(path, data, size, tempPath, error))
		return false;
	FileIdentity prepared{};
	if (!readIdentity(tempPath, prepared))
	{
		error = "prepared temporary file disappeared before publish";
		::unlink(tempPath.c_str());
		return false;
	}

#ifdef ATOMIC_FILE_TESTING
	{
		auto hook = std::move(detail::beforeConditionalPublishHook());
		detail::beforeConditionalPublishHook() = nullptr;
		if (hook) hook();
	}
#endif

	if (!detail::renameExchange(tempPath, path))
	{
		const int exchangeError = errno;
		error = "conditional publish unavailable: " +
		         std::string(std::strerror(exchangeError));
		::unlink(tempPath.c_str());
		return false;
	}

#ifdef ATOMIC_FILE_TESTING
	{
		auto hook = std::move(detail::beforeConditionalRollbackHook());
		detail::beforeConditionalRollbackHook() = nullptr;
		if (hook) hook();
	}
#endif

	FileIdentity displaced{};
	FileIdentity visible{};
	const bool displacedMatches =
		readIdentity(tempPath, displaced) && displaced == expected;
	const bool preparedVisible =
		readIdentity(path, visible) && visible == prepared;
	if (displacedMatches && preparedVisible)
	{
		::unlink(tempPath.c_str());
		detail::syncParent(path);
		return true;
	}

	if (!preparedVisible)
	{
		const bool preserved = detail::preserveConflict(tempPath, path);
		error = "file changed during conditional publish before rollback";
		if (!preserved)
		{
			error += "; external inode preservation failed: ";
			error += std::strerror(errno);
		}
		return false;
	}

	if (!detail::renameExchange(tempPath, path))
	{
		const int rollbackError = errno;
		const bool preserved = detail::preserveConflict(tempPath, path);
		error = "file changed during conditional publish; conditional publish "
		        "rollback failed: " + std::string(std::strerror(rollbackError));
		if (!preserved)
		{
			error += "; external inode preservation failed: ";
			error += std::strerror(errno);
		}
		return false;
	}

	FileIdentity rollbackTemp{};
	const bool preparedRolledBack =
		readIdentity(tempPath, rollbackTemp) && rollbackTemp == prepared;
	if (preparedRolledBack)
	{
		// The rollback left the external displaced inode at `path` and our
		// prepared inode at `tempPath`; discard only the verified inode we own.
		::unlink(tempPath.c_str());
		detail::syncParent(path);
		error = "file changed during conditional publish";
		return false;
	}

	// An external writer won the window around the rollback exchange. Put its
	// inode back at the target when possible, then preserve the displaced inode
	// under a no-replace conflict name. Never unlink an identity we did not
	// verify as our prepared inode.
	if (detail::renameExchange(tempPath, path))
	{
		const bool preserved = detail::preserveConflict(tempPath, path);
		error = "file changed during conditional publish; rollback target changed";
		if (!preserved)
		{
			error += "; external inode preservation failed: ";
			error += std::strerror(errno);
		}
		return false;
	}

	const int restoreError = errno;
	const bool preserved = detail::preserveConflict(tempPath, path);
	error = "file changed during conditional publish; rollback target changed: " +
	        std::string(std::strerror(restoreError));
	if (!preserved)
	{
		error += "; external inode preservation failed: ";
		error += std::strerror(errno);
	}
	return false;
}

inline bool writeIfUnchanged(const std::string& path,
                             const FileIdentity& expected,
                             const std::string& contents,
                             std::string& error)
{
	return writeIfUnchanged(path, expected, contents.data(), contents.size(),
	                        error);
}

inline bool write(const std::string& path, const std::string& contents,
                 std::string& error)
{
	return write(path, contents.data(), contents.size(), error);
}

} // namespace AtomicFile
