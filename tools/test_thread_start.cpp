// Worker-thread startup and the exception barrier around detached workers.

#include "thread_start.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}

	// std::thread copies its worker before the thread exists, so a throwing
	// copy fails the start the way a thread that cannot be created does.
	struct ThrowingCopy
	{
		ThrowingCopy() = default;
		ThrowingCopy(const ThrowingCopy&)
		{
			throw std::runtime_error("simulated thread failure");
		}
		void operator()() const {}
	};
}

int main()
{
	std::atomic<bool> ran{false};
	bool reset = false;
	check(ThreadStart::startDetached(
	              [&] { ran.store(true, std::memory_order_release); },
	              [&] { reset = true; }),
	      "a started worker reports success");
	for (int i = 0; i < 1000 && !ran.load(std::memory_order_acquire); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	check(ran.load(std::memory_order_acquire) && !reset,
	      "a started worker runs and does not reset state");

	reset = false;
	ThrowingCopy worker;
	check(!ThreadStart::startDetached(worker, [&] { reset = true; }),
	      "a worker that cannot start reports failure");
	check(reset, "a worker that cannot start invokes the reset callback");

	bool bodyRan = false;
	bool bodyFailure = false;
	bool bodyReset = false;
	ThreadStart::runGuarded(
		[&]
		{
			bodyRan = true;
			throw std::runtime_error("simulated detached worker failure");
		},
		[&] { bodyFailure = true; },
		[&] { bodyReset = true; });
	check(bodyRan, "guarded detached body runs after thread creation");
	check(bodyFailure, "guarded detached body catches its exception");
	check(bodyReset, "guarded detached body resets state after failure");

	bodyFailure = false;
	bodyReset = false;
	ThreadStart::runGuarded(
		[] {},
		[&] { bodyFailure = true; },
		[&] { bodyReset = true; });
	check(!bodyFailure, "guarded detached body does not report normal exit as failure");
	check(bodyReset, "guarded detached body resets state after normal exit");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_thread_start: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("thread start tests passed");
	return 0;
}
