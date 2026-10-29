// Standalone regression test for the manifest-fetch worker pool.
//
// The production bug created one std::async thread per manifest.  A title with
// hundreds of depots therefore exhausted the 32-bit Steam process even when
// every manifest was already cached.  The executor must accept any queue
// length while bounding only simultaneous work.
//
// Build (from repo root):
//   g++ -std=c++20 -pthread tools/test_boundedexecutor.cpp -o /tmp/test_boundedexecutor && /tmp/test_boundedexecutor

#include "../src/utils/boundedexecutor.hpp"
#include "../src/utils/ManifestFetch.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	constexpr std::size_t kWorkers = 4;
	constexpr int kJobs = 2000;

	BoundedExecutor executor(kWorkers);
	std::atomic<int> active{0};
	std::atomic<int> maxActive{0};
	std::atomic<int> completed{0};
	std::mutex doneLock;
	std::condition_variable doneCv;
	bool allAccepted = true;

	for (int i = 0; i < kJobs; ++i)
	{
		allAccepted = executor.submit([&]
		{
			const int now = active.fetch_add(1) + 1;
			int observed = maxActive.load();
			while (now > observed &&
			       !maxActive.compare_exchange_weak(observed, now))
			{
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			active.fetch_sub(1);
			completed.fetch_add(1);
			doneCv.notify_one();
		}) && allAccepted;
	}

	{
		std::unique_lock<std::mutex> lk(doneLock);
		doneCv.wait_for(lk, std::chrono::seconds(10), [&]
		{
			return completed.load() == kJobs;
		});
	}

	CHECK(executor.workerCount() == kWorkers,
	      "executor: starts the configured fixed worker count");
	CHECK(allAccepted, "executor: accepts all jobs without a queue-length cap");
	CHECK(completed.load() == kJobs,
	      "executor: processes every queued manifest job");
	CHECK(maxActive.load() <= static_cast<int>(kWorkers),
	      "executor: never exceeds the configured concurrency");
	CHECK(maxActive.load() > 1,
	      "executor: still runs independent manifest jobs concurrently");

	{
		BoundedExecutor capped(1, 1);
		std::mutex gateLock;
		std::condition_variable gateCv;
		std::atomic<bool> firstStarted{false};
		std::atomic<bool> releaseFirst{false};
		const bool firstAccepted = capped.submit([&]
		{
			firstStarted.store(true, std::memory_order_release);
			gateCv.notify_one();
			while (!releaseFirst.load(std::memory_order_acquire))
				std::this_thread::yield();
		});
		{
			std::unique_lock<std::mutex> lk(gateLock);
			gateCv.wait_for(lk, std::chrono::seconds(1), [&]
			{
				return firstStarted.load(std::memory_order_acquire);
			});
		}
		const bool secondAccepted = capped.submit([] {});
		const bool thirdAccepted = capped.submit([] {});
		CHECK(firstAccepted && secondAccepted && !thirdAccepted,
		      "executor: configured queue cap rejects excess work");
		releaseFirst.store(true, std::memory_order_release);
	}

	ManifestFetch::JobBudget budget(std::chrono::milliseconds(100));
	CHECK(!budget.shouldStop(), "manifest budget starts active");
	budget.cancel();
	CHECK(budget.shouldStop(), "manifest budget cancellation is observable");
	ManifestFetch::JobBudget shortBudget(std::chrono::milliseconds(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(shortBudget.shouldStop(), "manifest budget expires deterministically");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
