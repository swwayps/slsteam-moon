// Regression tests for startup provisioning network policy.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_provision_network.cpp -o /tmp/t && /tmp/t

#include "../src/feats/provision_network.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using AppInfoProvision::NetworkFailure;
	using AppInfoProvision::ProvisionPassState;
	using AppInfoProvision::classifyCurlFailure;
	using AppInfoProvision::classifyHttpFailure;
	using AppInfoProvision::retryNetworkOperation;

	CHECK(classifyCurlFailure(CURLE_COULDNT_RESOLVE_HOST) ==
	          NetworkFailure::Connectivity,
	      "DNS failure is classified as connectivity loss");
	CHECK(classifyCurlFailure(CURLE_COULDNT_CONNECT) ==
	          NetworkFailure::Connectivity,
	      "connect failure is classified as connectivity loss");
	CHECK(classifyCurlFailure(CURLE_OPERATION_TIMEDOUT) ==
	          NetworkFailure::Transient,
	      "timeout remains retryable");
	CHECK(classifyHttpFailure(503) == NetworkFailure::Transient,
	      "HTTP 503 remains retryable");
	CHECK(classifyHttpFailure(404) == NetworkFailure::Provider,
	      "HTTP 404 is a provider response, not an offline signal");

	// A deterministic connectivity failure must not sleep/retry. Reintroducing
	// generic bool retries would make this issue three attempts again.
	{
		int attempts = 0;
		std::vector<int> sleeps;
		const auto result = retryNetworkOperation(
		    [&] {
			    ++attempts;
			    return NetworkFailure::Connectivity;
		    },
		    3, 100, [&](int ms) { sleeps.push_back(ms); });
		CHECK(result == NetworkFailure::Connectivity,
		      "DNS failure is returned to the pass");
		CHECK(attempts == 1, "DNS failure makes exactly one attempt");
		CHECK(sleeps.empty(), "DNS failure never enters backoff");
	}

	// A timeout can still be a cold/slow provider, so retain bounded retries.
	{
		int attempts = 0;
		std::vector<int> sleeps;
		const auto result = retryNetworkOperation(
		    [&] {
			    ++attempts;
			    return attempts == 3 ? NetworkFailure::None
			                         : NetworkFailure::Transient;
		    },
		    3, 100, [&](int ms) { sleeps.push_back(ms); });
		CHECK(result == NetworkFailure::None,
		      "transient provider failure can recover");
		CHECK(attempts == 3, "transient failure uses the bounded retry budget");
		CHECK(sleeps == std::vector<int>({100, 200}),
		      "transient retry keeps linear backoff");
	}

	// Once both the primary CM batch and the final provider fail at transport
	// level, later apps in this startup pass must not touch the network.
	{
		ProvisionPassState pass;
		pass.noteCmBatchFailure();
		pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
		CHECK(pass.providerCircuitOpen(),
		      "connectivity failure opens the pass-wide provider circuit");
		CHECK(!pass.shouldAttemptProvider(),
		      "later apps skip providers after the circuit opens");
	}
	{
		ProvisionPassState pass;
		pass.noteCmBatchFailure();
		pass.noteFinalProviderFailure(NetworkFailure::Transient);
		CHECK(pass.providerCircuitOpen(),
		      "exhausted transport timeout also opens the provider circuit");
		CHECK(pass.takeConnectivityNotice(),
		      "the first app without cache may emit the connectivity notice");
		CHECK(!pass.takeConnectivityNotice(),
		      "later apps do not repeat the fleet-wide connectivity notice");
	}

	// If CM failed for a reason other than global connectivity but a fallback
	// request succeeds, retry CM once for the remaining fleet instead of
	// serially keeping every app on the slow mirror.
	{
		ProvisionPassState pass;
		pass.noteCmBatchFailure();
		pass.noteProviderSuccess();
		CHECK(pass.takeCmRecoveryRequest(),
		      "provider success requests one CM recovery batch");
		CHECK(!pass.takeCmRecoveryRequest(),
		      "CM recovery request is consumed only once");
	}

	// The fallback budget is lazy (CM time does not consume it), shared by
	// every app in the pass, and bounds both transfers and retry sleeps.
	{
		ProvisionPassState pass;
		CHECK(!pass.providerBudgetExhausted(),
		      "provider budget is untouched before the first fallback");
		CHECK(pass.providerOperationTimeoutMs(12000) == 12000,
		      "first fallback operation keeps its per-operation cap");
		CHECK(pass.providerDelayMs(20000) <= ProvisionPassState::kProviderBudgetMs,
		      "retry delay is capped by the shared provider budget");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		CHECK(pass.providerOperationTimeoutMs(12000) <= 12000,
		      "later fallback operations never regain spent time");
	}

	if (g_failures == 0) std::printf("\nall provision-network checks passed\n");
	else std::printf("\n%d provision-network check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
