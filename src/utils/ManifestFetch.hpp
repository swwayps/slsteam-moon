
#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>


namespace ManifestFetch
{
	// Pure request-code circuit policy. Time is supplied by the caller in
	// monotonic milliseconds, keeping provider cooldown behavior deterministic
	// in host tests. An open circuit admits exactly one REAL gid after cooldown;
	// there is no synthetic gid=0 health probe whose 404 could look healthy.
	class RequestCodeCircuit
	{
	public:
		RequestCodeCircuit(int failureThreshold, std::int64_t cooldownMs)
			: m_threshold(failureThreshold > 0 ? failureThreshold : 1),
			  m_cooldownMs(cooldownMs > 0 ? cooldownMs : 1) {}

		bool beginAttempt(std::int64_t nowMs)
		{
			if (!m_open) return true;
			if (nowMs < m_retryAtMs || m_halfOpenInFlight) return false;
			m_halfOpenInFlight = true;
			return true;
		}

		void finishAttempt(std::int64_t nowMs, bool success,
		                   bool rateLimited, bool transportFailure)
		{
			if (success)
			{
				m_open = false;
				m_halfOpenInFlight = false;
				m_consecutiveTransportFailures = 0;
				m_retryAtMs = 0;
				return;
			}

			if (m_open && m_halfOpenInFlight)
			{
				m_halfOpenInFlight = false;
				m_retryAtMs = nowMs + m_cooldownMs;
				return;
			}

			if (rateLimited)
			{
				openAt(nowMs);
				return;
			}
			if (transportFailure)
			{
				if (++m_consecutiveTransportFailures >= m_threshold)
					openAt(nowMs);
			}
			else
			{
				m_consecutiveTransportFailures = 0;
			}
		}

		bool open() const noexcept { return m_open; }

	private:
		void openAt(std::int64_t nowMs)
		{
			m_open = true;
			m_halfOpenInFlight = false;
			m_retryAtMs = nowMs + m_cooldownMs;
		}

		int m_threshold;
		std::int64_t m_cooldownMs;
		int m_consecutiveTransportFailures = 0;
		std::int64_t m_retryAtMs = 0;
		bool m_open = false;
		bool m_halfOpenInFlight = false;
	};

	struct ProviderOutcome
	{
		bool networkError = false;
		long httpStatus = 0;
	};

	// A gid is absent only when every provider we actually reached answered
	// 404. A 429, transport error, server error, or invalid 200 leaves the
	// result unknown and must not poison the session-wide not-found cache.
	inline bool isDefinitiveNotFound(
	    const std::vector<ProviderOutcome>& outcomes)
	{
		if (outcomes.empty()) return false;
		for (const auto& outcome : outcomes)
		{
			if (outcome.networkError || outcome.httpStatus != 404) return false;
		}
		return true;
	}

	namespace detail
	{
		// Kill the whole helper process group and always reap the child.  This
		// is used on both timeout and waitpid-error paths so no unzip process
		// can outlive the manifest job and write into a removed temp file.
		inline void killAndReap(pid_t pid, int& status) noexcept
		{
			if (pid <= 0) return;
			if (kill(-pid, SIGKILL) != 0) (void)kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		}

		// The caller must hold the in-flight-job mutex while invoking this.
		// Keeping registration beside lookup/creation prevents a last waiter
		// from cancelling a job before a concurrent joiner is counted.
		inline void registerWaiterLocked(std::atomic<int>& waiters) noexcept
		{
			waiters.fetch_add(1, std::memory_order_acq_rel);
		}

		inline bool releaseWaiterLocked(std::atomic<int>& waiters) noexcept
		{
			return waiters.fetch_sub(1, std::memory_order_acq_rel) == 1;
		}
		inline bool shouldCancelBlobJob(bool producerActive, bool lastWaiter,
		                                bool cancelIfLast) noexcept
		{
			return cancelIfLast && lastWaiter && !producerActive;
		}

	} // namespace detail

	// A blob job has one deadline shared by all callers. A fire-and-forget
	// producer owns the job until completion; a caller timeout can cancel only
	// after that producer has released ownership and the caller is last. Queued
	// work also expires without ever entering a worker thread.
	struct JobBudget
	{
		explicit JobBudget(std::chrono::milliseconds duration)
			: deadline(std::chrono::steady_clock::now() + duration) {}

		void cancel() noexcept
		{
			cancelled.store(true, std::memory_order_release);
		}

		bool shouldStop() const noexcept
		{
			return cancelled.load(std::memory_order_acquire)
				|| std::chrono::steady_clock::now() >= deadline;
		}

		std::chrono::milliseconds remaining() const noexcept
		{
			if (cancelled.load(std::memory_order_acquire))
				return std::chrono::milliseconds(0);
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline) return std::chrono::milliseconds(0);
			return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		}

		std::chrono::steady_clock::time_point deadline;
		std::atomic<bool> cancelled{false};
	};

	// One CDN host fetch outcome, reduced to the two facts the expired-code
	// retry policy cares about.
	struct CdnOutcome
	{
		bool networkError; // curl itself failed (status is meaningless)
		long httpStatus;   // HTTP response code when networkError == false
	};

	// True iff at least one host was tried and EVERY attempt failed
	// specifically with HTTP 401 (Unauthorized) and none failed for a
	// different reason.  A unanimous 401 across all CDN hosts is the
	// signature of an expired manifest request-code (codes carry a ~5-min
	// CDN TTL) and is recoverable by re-resolving a fresh code.  Any network
	// error or non-401 status (e.g. a 503 overloaded edge) is a real or
	// transient failure that re-resolving the code would not fix, so we must
	// NOT burn a provider round-trip on it.
	inline bool isExpiredCodeSignature(const std::vector<CdnOutcome>& outcomes)
	{
		if (outcomes.empty()) return false;
		for (const auto& o : outcomes)
		{
			if (o.networkError || o.httpStatus != 401) return false;
		}
		return true;
	}

	int getTimeoutSec();
	const char* defaultTimeoutKey();

	void submit(uint64_t jobId, uint64_t manifestGid,
	            uint32_t appId, uint32_t depotId);

	void submitManifestBlob(uint64_t manifestGid,
	                        uint32_t appId, uint32_t depotId);

	// Submit (or join an in-flight one) and block up to timeoutSec for the
	// manifest blob to land on disk.  Returns true if the .manifest file is
	// now present (or was already there).  Used by BYldRequestDepotManifest
	// to avoid the "no internet / hit retry" UX: Steam's first install
	// attempt finds the file already cached.
	bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId,
	                       int timeoutSec);

	// Join the exact blob job for at most timeoutMs. Used by the real Steam
	// install plan so every depot shares one total deadline instead of each
	// blocking a Steam worker for a fresh 12 seconds.
	bool awaitManifestBlobFor(uint64_t manifestGid, uint32_t depotId,
	                          int timeoutMs, bool notifyOnTimeout);

	bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t depotId);

	std::optional<uint64_t> resolve(uint64_t jobId);

	void discard(uint64_t jobId);

	bool areProvidersOffline();
	bool isGidNotFound(uint64_t gid);
	void markGidNotFound(uint64_t gid);
}
