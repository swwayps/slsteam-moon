#pragma once

#include <thread>
#include <utility>
namespace ThreadStart
{
	template <typename Callback>
	void invokeNoThrow(Callback&& callback) noexcept
	{
		try
		{
			callback();
		}
		catch (...)
		{
			// Recovery callbacks must not escape a worker-start boundary.
		}
	}

	// Start a detached worker without letting a failed start escape into
	// Steam. onFailure restores the caller's state so a later attempt can
	// retry and waiters cannot block forever. detach() cannot fail on a
	// thread that was just created.
	template <typename Worker, typename OnFailure>
	bool startDetached(Worker&& worker, OnFailure&& onFailure) noexcept
	{
		try
		{
			std::thread(std::forward<Worker>(worker)).detach();
			return true;
		}
		catch (...)
		{
			invokeNoThrow(std::forward<OnFailure>(onFailure));
			return false;
		}
	}

	// Run the body of a detached worker behind a top-level exception barrier.
	// Cleanup runs after both normal return and failure; each callback is
	// isolated so state recovery cannot rethrow into the thread runtime.
	template <typename Worker, typename OnFailure, typename OnExit>
	void runGuarded(Worker&& worker, OnFailure&& onFailure,
	                OnExit&& onExit) noexcept
	{
		try
		{
			worker();
		}
		catch (...)
		{
			try
			{
				onFailure();
			}
			catch (...)
			{
				// Failure reporting must not prevent state recovery.
			}
		}

		try
		{
			onExit();
		}
		catch (...)
		{
			// Cleanup must not escape the detached worker either.
		}
	}
} // namespace ThreadStart
