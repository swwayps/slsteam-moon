// TDD regression test for recoverable worker-thread startup.

#include "thread_start.hpp"

#include <atomic>
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
}

int main()
{
	bool started = false;
	bool reset = false;
	check(ThreadStart::tryStart(
	              [&] { started = true; },
	              [&] { reset = true; }),
	      "successful starter reports success");
	check(started && !reset, "successful starter does not reset state");

	started = false;
	reset = false;
	check(!ThreadStart::tryStart(
	              [&] { throw std::runtime_error("simulated thread failure"); },
	              [&] { reset = true; }),
	      "throwing starter reports failure");
	check(!started && reset, "throwing starter invokes reset callback");


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

	bool detachReset = false;
	bool detachFailure = false;
	bool workerRan = false;
	struct ThrowingDetacher
	{
		void operator()(std::thread&) const
		{
			throw std::runtime_error("simulated detach failure");
		}
	};
	check(!ThreadStart::startDetached(
		[&] { workerRan = true; },
		[&] { detachReset = true; },
		[&] { detachFailure = true; },
		ThrowingDetacher{}),
		"detach failure reports worker-start failure");
	struct ThrowingJoiner
	{
		std::atomic<bool>* attempted;

		void operator()(std::thread&) const
		{
			attempted->store(true, std::memory_order_release);
			throw std::runtime_error("simulated join failure");
		}
	};
	std::atomic<bool> joinAttempted{false};
	std::atomic<bool> joinWorkerRan{false};
	bool joinReset = false;
	bool joinRecovery = false;
	check(!ThreadStart::startDetached(
		[&] { joinWorkerRan.store(true, std::memory_order_release); },
		[&] { joinReset = true; },
		[] {},
		ThrowingDetacher{},
		ThrowingJoiner{&joinAttempted},
		[&] { joinRecovery = true; }),
		"join failure reports worker-start failure");
	for (int i = 0; i < 100 && !joinWorkerRan.load(std::memory_order_acquire); ++i)
		std::this_thread::yield();
	check(joinAttempted.load(std::memory_order_acquire),
	      "join failure invokes the injected joiner");
	check(joinWorkerRan.load(std::memory_order_acquire)
	      && joinRecovery && !joinReset,
	      "join failure does not reopen state before worker exit");

	std::atomic<bool> recoveryWorkerStarted{false};
	std::atomic<bool> recoveryWorkerFinished{false};
	std::atomic<bool> recoveryRelease{false};
	std::atomic<bool> recoveryJoinAttempted{false};
	bool recoveryReset = false;
	bool recoveryUncertain = false;
	check(!ThreadStart::startDetached(
		[&]
		{
			recoveryWorkerStarted.store(true, std::memory_order_release);
			while (!recoveryRelease.load(std::memory_order_acquire))
				std::this_thread::yield();
			recoveryWorkerFinished.store(true, std::memory_order_release);
		},
		[&] { recoveryReset = true; },
		[] {},
		ThrowingDetacher{},
		ThrowingJoiner{&recoveryJoinAttempted},
		[&] { recoveryUncertain = true; }),
		"unrecoverable detach failure reports failure");
	for (int i = 0; i < 1000
	     && !recoveryWorkerStarted.load(std::memory_order_acquire); ++i)
		std::this_thread::yield();
	check(recoveryJoinAttempted.load(std::memory_order_acquire),
	      "unrecoverable path attempts join");
	check(recoveryUncertain && !recoveryReset,
	      "unrecoverable path does not reopen state before worker exit");
	recoveryRelease.store(true, std::memory_order_release);
	for (int i = 0; i < 1000
	     && !recoveryWorkerFinished.load(std::memory_order_acquire); ++i)
		std::this_thread::yield();
	check(recoveryWorkerFinished.load(std::memory_order_acquire),
	      "unrecoverable worker eventually exits");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_thread_start: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("thread start tests passed");
	return 0;
}
