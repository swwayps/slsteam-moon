#include "../src/filewatcher_burst.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace
{
int failures = 0;

#define CHECK(condition, message) do {                                      \
	if (!(condition)) { std::printf("FAIL: %s\n", message); ++failures; }  \
	else              { std::printf("ok:   %s\n", message); }              \
} while (0)

void quietWindowCoalescesOneBurst()
{
	int pipeFds[2]{};
	CHECK(pipe(pipeFds) == 0, "burst fixture pipe created");
	const char first = 'a';
	CHECK(write(pipeFds[1], &first, 1) == 1, "initial readable byte queued");

	char byte = 0;
	CHECK(read(pipeFds[0], &byte, 1) == 1, "initial readable byte consumed");
	std::thread writer([&] {
		for (int i = 0; i < 5; ++i)
		{
			std::this_thread::sleep_for(15ms);
			const char next = static_cast<char>('b' + i);
			(void)write(pipeFds[1], &next, 1);
		}
	});

	const auto start = std::chrono::steady_clock::now();
	const auto drained = FileWatcherBurst::drainAdditionalReadable(
		pipeFds[0], /*quietWindowMs=*/40, /*maxWindowMs=*/500);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	writer.join();
	close(pipeFds[0]);
	close(pipeFds[1]);

	CHECK(drained == 5, "all writes in one burst are drained together");
	CHECK(elapsed >= 90ms, "quiet window restarts after each arriving write");
}

void maximumWindowBoundsContinuousTraffic()
{
	int pipeFds[2]{};
	CHECK(pipe(pipeFds) == 0, "continuous fixture pipe created");
	const char first = 'a';
	CHECK(write(pipeFds[1], &first, 1) == 1, "continuous initial byte queued");
	char byte = 0;
	CHECK(read(pipeFds[0], &byte, 1) == 1, "continuous initial byte consumed");

	std::thread writer([&] {
		for (int i = 0; i < 30; ++i)
		{
			std::this_thread::sleep_for(10ms);
			(void)write(pipeFds[1], &first, 1);
		}
	});

	const auto start = std::chrono::steady_clock::now();
	const auto drained = FileWatcherBurst::drainAdditionalReadable(
		pipeFds[0], /*quietWindowMs=*/50, /*maxWindowMs=*/100);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	writer.join();
	close(pipeFds[0]);
	close(pipeFds[1]);

	CHECK(drained > 0 && drained < 30,
	      "bounded coalescing returns before continuous traffic ends");
	CHECK(elapsed >= 80ms && elapsed < 220ms,
	      "continuous traffic is bounded by the maximum window");
}
} // namespace

int main()
{
	quietWindowCoalescesOneBurst();
	maximumWindowBoundsContinuousTraffic();
	if (failures == 0) std::puts("all filewatcher burst checks passed");
	return failures == 0 ? 0 : 1;
}
