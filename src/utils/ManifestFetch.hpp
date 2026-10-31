
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>


namespace ManifestFetch
{
	inline bool appStateIsDownloading(uint32_t state) noexcept
	{
		return (state & (0x100u | 0x400u)) != 0;
	}

	// Remembers one answer for a short window. The scan calls Steam, so it runs
	// outside the lock: a caller that holds a Steam lock must never wait for a
	// scan that waits for that lock. Callers in a stale window may both scan.
	class DownloadActivityCache
	{
	public:
		static constexpr std::int64_t kWindowMs = 1000;

		template <typename Scan>
		bool get(std::int64_t nowMs, Scan&& scan)
		{
			{
				std::lock_guard lock(m_mutex);
				if (m_valid && nowMs - m_checkedAtMs < kWindowMs) return m_active;
			}
			const bool active = scan();
			std::lock_guard lock(m_mutex);
			m_active = active;
			m_checkedAtMs = nowMs;
			m_valid = true;
			return active;
		}

	private:
		std::mutex m_mutex;
		bool m_valid = false;
		bool m_active = false;
		std::int64_t m_checkedAtMs = 0;
	};

	// Availability signal fed to the provider circuit from a luastools archive
	// fetch. `success` means the host answered (so we are online), independent
	// of whether it held the manifest.
	struct ArchiveOutcome
	{
		bool success;
		bool rateLimited;
		bool transportFailure;
	};

	// Classify a luastools archive HTTP result for the offline circuit. The
	// archive is reachable whenever it answers at all — including a 404, which
	// only means the manifest has not been donated yet, not that we are offline.
	// A transport error or a 5xx means the host is unreachable; 429 is a rate
	// limit that opens the circuit immediately. A transfer the job's own budget
	// stopped (`stoppedByUs`) says nothing either way.
	inline ArchiveOutcome classifyArchiveOutcome(bool networkError,
	                                              long httpStatus,
	                                              bool stoppedByUs = false) noexcept
	{
		if (networkError && stoppedByUs) return {false, false, false};
		if (networkError) return {false, false, true};
		if (httpStatus == 429) return {false, true, false};
		if (httpStatus >= 500) return {false, false, true};
		return {true, false, false};
	}

	// The request may carry a DLC app id while only its base AddedApp has the
	// active state. Check the hint plus every managed base app, matching Steam's
	// UI-level "any active download" distinction used by the MRC flow.
	bool isAnyManagedDownloadActive(uint32_t appIdHint);

	struct HttpResponse
	{
		long status = 0;
		std::string body;
		bool networkError = false;
		std::string diagnostic;
	};

	// Bounded, signal-free HTTP for the manifest archive protocol.
	HttpResponse archiveRequest(const std::string& method, const std::string& url,
	                            const std::string& body = {},
	                            std::size_t maxBodyBytes = 32u * 1024u * 1024u,
	                            long timeoutMs = 30000,
	                            const std::atomic<bool>* keepRunning = nullptr);

	inline bool looksLikeArchivedManifest(std::string_view body)
	{
		static constexpr std::string_view payload{"\xd0\x17\xf6\x71", 4};
		static constexpr std::string_view eof{"\xab\x15\xc4\x32", 4};
		return body.size() >= 16 && body.substr(0, 4) == payload
		       && body.substr(body.size() - 4) == eof;
	}
	// Pure availability circuit for the sole manifest provider (the luastools
	// archive). Time is supplied by the caller in monotonic milliseconds,
	// keeping cooldown behavior deterministic in host tests. An open circuit
	// admits exactly one real probe after cooldown; recovery is proven only by
	// a reachable response, so a rate limit or transport outage can never look
	// healthy on its own.
	class ProviderCircuit
	{
	public:
		ProviderCircuit(int failureThreshold, std::int64_t cooldownMs)
			: m_threshold(failureThreshold > 0 ? failureThreshold : 1),
			  m_cooldownMs(cooldownMs > 0 ? cooldownMs : 1) {}

		std::uint64_t beginAttempt(std::int64_t nowMs)
		{
			if (!m_open) return nextToken();
			if (nowMs < m_retryAtMs || m_halfOpenInFlight) return 0;
			m_halfOpenInFlight = true;
			m_halfOpenToken = nextToken();
			return m_halfOpenToken;
		}

		void finishAttempt(std::uint64_t token, std::int64_t nowMs, bool success,
		                   bool rateLimited, bool transportFailure)
		{
			if (!token || (m_halfOpenInFlight && token != m_halfOpenToken))
				return;
			if (success)
			{
				m_open = false;
				m_halfOpenInFlight = false;
				m_halfOpenToken = 0;
				m_consecutiveTransportFailures = 0;
				m_retryAtMs = 0;
				return;
			}

			if (m_open && m_halfOpenInFlight)
			{
				m_halfOpenInFlight = false;
				m_halfOpenToken = 0;
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

		void cancelAttempt(std::uint64_t token) noexcept
		{
			if (!token || token != m_halfOpenToken) return;
			m_halfOpenInFlight = false;
			m_halfOpenToken = 0;
		}


		bool open() const noexcept { return m_open; }
		// When an open circuit admits its probe.
		std::int64_t retryAtMs() const noexcept { return m_retryAtMs; }
		// Open and not yet due for its probe. Updates are held back only for
		// this window: after it, the next update plan is the probe.
		bool coolingDown(std::int64_t nowMs) const noexcept
		{
			return m_open && nowMs < m_retryAtMs;
		}
		void reset() noexcept
		{
			m_open = false;
			m_halfOpenInFlight = false;
			m_halfOpenToken = 0;
			m_consecutiveTransportFailures = 0;
			m_retryAtMs = 0;
		}

	private:
		void openAt(std::int64_t nowMs)
		{
			m_open = true;
			m_halfOpenInFlight = false;
			m_halfOpenToken = 0;
			m_retryAtMs = nowMs + m_cooldownMs;
		}

		std::uint64_t nextToken() noexcept
		{
			if (++m_nextToken == 0) ++m_nextToken;
			return m_nextToken;
		}

		int m_threshold;
		std::int64_t m_cooldownMs;
		int m_consecutiveTransportFailures = 0;
		std::int64_t m_retryAtMs = 0;
		bool m_open = false;
		bool m_halfOpenInFlight = false;
		std::uint64_t m_halfOpenToken = 0;
		std::uint64_t m_nextToken = 0;
	};

	namespace detail
	{
		inline void promoteArchiveMissBypass(std::atomic<bool>& promoted,
		                                     bool requested) noexcept
		{
			if (requested) promoted.store(true, std::memory_order_release);
		}
	} // namespace detail

	// A blob job has one deadline shared by all callers. A caller that times
	// out leaves the job running so the manifest is staged for Steam's retry;
	// queued work also expires without ever entering a worker thread.
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

	// Blob jobs are keyed by depot and gid, the archive's address.
	void submitManifestBlob(uint64_t manifestGid, uint32_t depotId,
	                        bool bypassArchiveMiss = false);

	// Submit (or join an in-flight one) and block up to 12 s for the
	// manifest blob to land on disk.  Returns true if the .manifest file is
	// now present (or was already there).  Used by BYldRequestDepotManifest
	// to avoid the "no internet / hit retry" UX: Steam's first install
	// attempt finds the file already cached.
	bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId,
	                       bool activeRequest = false);

	// Join the exact blob job for at most timeoutMs. Used by the real Steam
	// install plan so every depot shares one total deadline instead of each
	// blocking a Steam worker for a fresh 12 seconds.
	bool awaitManifestBlobFor(uint64_t manifestGid, uint32_t depotId,
	                          int timeoutMs, bool notifyOnTimeout,
	                          bool activeRequest = false);


	void resetSessionState();

	bool areProvidersOffline();

	// Record whether a depot is installed from a local manifest instead of its
	// public one; offline_<appId> exists while any depot of the game is.
	void setDepotOnFallback(uint32_t appId, uint32_t depotId, bool onFallback);

	// Remove the offline flags of the previous session. Called from load(), so
	// only the client that runs the session clears them.
	void clearOfflineFlags();
}
