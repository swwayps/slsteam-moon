#pragma once

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AuditLog
{
	inline std::size_t textLength(const char* text) noexcept
	{
		const char* end = text;
		while (*end != '\0')
		{
			++end;
		}
		return static_cast<std::size_t>(end - text);
	}

	// The log descriptor and the file it was opened on.
	struct Sink
	{
		int fd = -1;
		dev_t dev = 0;
		ino_t ino = 0;
	};

	// Open `path` for appending and remember which file that is.
	inline bool reopen(Sink& sink, const char* path) noexcept
	{
		sink.fd = -1;
		const int fd = ::open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
		if (fd < 0)
		{
			return false;
		}
		struct stat st {};
		if (::fstat(fd, &st) != 0)
		{
			::close(fd);
			return false;
		}
		sink = {fd, st.st_dev, st.st_ino};
		return true;
	}

	// The descriptor still refers to the file it was opened on. A number
	// that now names another file is not ours to close or write.
	inline bool stillOpen(const Sink& sink) noexcept
	{
		struct stat st {};
		return sink.fd >= 0 && ::fstat(sink.fd, &st) == 0 &&
		       st.st_dev == sink.dev && st.st_ino == sink.ino;
	}

	// Write a small diagnostic without consulting C++ runtime state (only
	// open, fstat and write). Steam may close inherited descriptors in a fork
	// child before the exec wrapper runs, or give the number to another file,
	// so the log is reopened before the same bytes are written.
	inline void write(Sink& sink, const char* path, const char* text) noexcept
	{
		if (path == nullptr || path[0] == '\0' || text == nullptr)
		{
			return;
		}

		if (!stillOpen(sink) && !reopen(sink, path))
		{
			return;
		}

		const char* cursor = text;
		std::size_t remaining = textLength(text);
		while (remaining > 0)
		{
			const ssize_t written = ::write(sink.fd, cursor, remaining);
			if (written < 0)
			{
				if (errno == EBADF)
				{
					if (!reopen(sink, path))
					{
						return;
					}
					continue;
				}
				if (errno == EINTR)
				{
					continue;
				}
				return;
			}
			if (written == 0)
			{
				return;
			}
			cursor += written;
			remaining -= static_cast<std::size_t>(written);
		}
	}
}
