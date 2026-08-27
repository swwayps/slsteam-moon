#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <poll.h>
#include <unistd.h>

namespace FileWatcherBurst
{

// The caller has already consumed the first readable batch. Drain every
// additional batch that arrives inside one quiet window, but return
// periodically under continuous traffic so configuration updates cannot be
// postponed forever. This turns a bulk directory copy into one logical reload
// instead of one reload per 4 KiB inotify read.
inline std::size_t drainAdditionalReadable(
	int fd, int quietWindowMs = 250, int maxWindowMs = 5000) noexcept
{
	if (fd < 0 || quietWindowMs <= 0 || maxWindowMs <= 0) return 0;

	using Clock = std::chrono::steady_clock;
	const auto started = Clock::now();
	std::size_t total = 0;
	alignas(std::max_align_t) char buffer[4096];

	for (;;)
	{
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - started).count();
		const int remainingMs = maxWindowMs - static_cast<int>(elapsedMs);
		if (remainingMs <= 0) return total;

		struct pollfd pfd{};
		pfd.fd = fd;
		pfd.events = POLLIN;
		int ready = 0;
		do
		{
			ready = poll(&pfd, 1, std::min(quietWindowMs, remainingMs));
		} while (ready < 0 && errno == EINTR);

		if (ready <= 0 || (pfd.revents & POLLIN) == 0) return total;

		ssize_t bytes = 0;
		do
		{
			bytes = read(fd, buffer, sizeof(buffer));
		} while (bytes < 0 && errno == EINTR);
		if (bytes <= 0) return total;
		total += static_cast<std::size_t>(bytes);
	}
}

} // namespace FileWatcherBurst
