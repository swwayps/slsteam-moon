// TDD regression test for prewarm idle-pass backoff.

#include "feats/prewarm.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

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
	Prewarm::PassBackoff backoff;
	const std::vector<Prewarm::DepotGid> first{{100, 10}};
	const std::vector<Prewarm::DepotGid> second{{100, 10}, {200, 20}};
	const auto fast = Prewarm::PassBackoff::kBaseInterval;
	const auto slow = Prewarm::PassBackoff::kBackoffInterval;

	check(backoff.interval() == fast, "initial interval is fast");
	check(backoff.observeTargets(first), "first target is new");
	backoff.recordPass(false);
	for (int i = 0; i < Prewarm::PassBackoff::kNoOpPassesBeforeBackoff; ++i)
	{
		check(!backoff.observeTargets(first), "unchanged target is not new");
		backoff.recordPass(true);
	}
	check(backoff.interval() == slow, "repeated all-blacklisted passes back off");
	check(slow <= std::chrono::minutes(1),
	      "a waiting install retries within one minute, not forever");

	check(backoff.observeTargets(second), "new target is detected");
	backoff.recordPass(false);
	check(backoff.interval() == fast, "new target resets to fast interval");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_prewarm_backoff: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("prewarm backoff tests passed");
	return 0;
}
