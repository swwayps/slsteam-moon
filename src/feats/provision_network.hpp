// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure startup-network policy for AppInfoProvision. The provisioner runs
// before Steam opens appinfo.vdf, so deterministic connectivity failures must
// stop the fleet-wide serial fallback immediately while genuinely transient
// provider failures retain a small retry budget.

#pragma once

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace AppInfoProvision
{

enum class NetworkFailure
{
	None,
	Connectivity,
	Transient,
	Provider,
};

inline NetworkFailure classifyCurlFailure(CURLcode code)
{
	switch (code)
	{
		case CURLE_OK:
			return NetworkFailure::None;
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_CONNECT:
		case CURLE_INTERFACE_FAILED:
			return NetworkFailure::Connectivity;
		case CURLE_OPERATION_TIMEDOUT:
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_GOT_NOTHING:
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
			return NetworkFailure::Transient;
		default:
			return NetworkFailure::Provider;
	}
}

inline NetworkFailure classifyHttpFailure(long status)
{
	if (status >= 500 || status == 408 || status == 429)
		return NetworkFailure::Transient;
	return status == 200 ? NetworkFailure::None : NetworkFailure::Provider;
}

inline NetworkFailure retryNetworkOperation(
    const std::function<NetworkFailure()>& op,
    int maxAttempts,
    int baseDelayMs,
    const std::function<void(int)>& sleepMs)
{
	if (maxAttempts < 1) maxAttempts = 1;
	NetworkFailure result = NetworkFailure::Provider;
	for (int attempt = 1; attempt <= maxAttempts; ++attempt)
	{
		result = op();
		if (result == NetworkFailure::None ||
		    result == NetworkFailure::Connectivity ||
		    result == NetworkFailure::Provider)
		{
			return result;
		}
		if (attempt < maxAttempts)
			sleepMs(baseDelayMs * attempt);
	}
	return result;
}

class ProvisionPassState
{
public:
	static constexpr long kProviderBudgetMs = 15000;

	void noteCmBatchFailure() { m_cmFailed = true; }

	void noteFinalProviderFailure(NetworkFailure failure)
	{
		if (failure == NetworkFailure::Connectivity ||
		    failure == NetworkFailure::Transient)
			m_providerCircuitOpen = true;
	}

	void noteProviderSuccess()
	{
		if (m_cmFailed && !m_recoveryAttempted)
			m_recoveryPending = true;
	}

	bool providerCircuitOpen() const { return m_providerCircuitOpen; }
	bool shouldAttemptProvider() const { return !m_providerCircuitOpen; }

	// The mirror is a startup contingency, not an unbounded per-app queue.
	// Start its pass-wide clock lazily so the native-CM budget does not consume
	// it, then cap every transfer and backoff to the remaining wall time.
	long providerOperationTimeoutMs(long operationCapMs)
	{
		startProviderBudget();
		return std::min(operationCapMs, remainingProviderBudgetMs());
	}

	long providerDelayMs(long requestedMs)
	{
		startProviderBudget();
		return std::min(requestedMs, remainingProviderBudgetMs());
	}

	bool providerBudgetExhausted() const
	{
		return m_providerBudgetStarted && remainingProviderBudgetMs() == 0;
	}

	bool takeCmRecoveryRequest()
	{
		if (!m_recoveryPending || m_recoveryAttempted) return false;
		m_recoveryPending = false;
		m_recoveryAttempted = true;
		return true;
	}

	bool takeConnectivityNotice()
	{
		if (m_connectivityNoticeTaken) return false;
		m_connectivityNoticeTaken = true;
		return true;
	}

	// Incomplete-metadata notices are aggregated instead of raised per app.
	// A bulk copy into stplug-in routinely contains hundreds of storefront-only,
	// DLC, demo or delisted entries with no content depots; one popup each is
	// unactionable and buries the single message that does matter. The first app
	// is still reported individually (the common "I just added one game" case),
	// and the rest are summarized once when the pass finishes.
	bool takePreparationNotice(uint32_t appId)
	{
		++m_preparationFailures;
		if (m_preparationNoticeTaken) return false;
		m_preparationNoticeTaken = true;
		m_firstPreparationFailure = appId;
		return true;
	}

	std::size_t preparationFailureCount() const { return m_preparationFailures; }

	// Number of apps that failed WITHOUT getting their own notification, i.e.
	// what the aggregated message must account for.
	std::size_t pendingPreparationSummary() const
	{
		return m_preparationFailures > 1 ? m_preparationFailures - 1 : 0;
	}

	uint32_t firstPreparationFailure() const { return m_firstPreparationFailure; }

private:
	using Clock = std::chrono::steady_clock;

	void startProviderBudget()
	{
		if (m_providerBudgetStarted) return;
		m_providerBudgetStarted = true;
		m_providerStarted = Clock::now();
	}

	long remainingProviderBudgetMs() const
	{
		if (!m_providerBudgetStarted) return kProviderBudgetMs;
		const long long elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        Clock::now() - m_providerStarted).count();
		if (elapsed >= kProviderBudgetMs) return 0;
		return static_cast<long>(kProviderBudgetMs - std::max(0LL, elapsed));
	}

	bool m_cmFailed = false;
	bool m_providerCircuitOpen = false;
	bool m_recoveryPending = false;
	bool m_recoveryAttempted = false;
	bool m_connectivityNoticeTaken = false;
	bool m_preparationNoticeTaken = false;
	std::size_t m_preparationFailures = 0;
	uint32_t m_firstPreparationFailure = 0;
	bool m_providerBudgetStarted = false;
	Clock::time_point m_providerStarted{};
};

} // namespace AppInfoProvision
