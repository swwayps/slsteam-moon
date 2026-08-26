#include "feats/installreadiness.hpp"

#include <cstdio>
#include <vector>

namespace
{
	int failures = 0;
	void check(bool condition, const char* message)
	{
		if (condition) std::printf("ok   %s\n", message);
		else { std::printf("FAIL %s\n", message); ++failures; }
	}
}

int main()
{
	using InstallReadiness::Observation;
	check(!InstallReadiness::shouldBlock(false, {10, 2, 0, false}),
	      "online providers never block the native wizard");
	check(!InstallReadiness::shouldBlock(true, {10, 0, 0, false}),
	      "unknown depot coverage fails open");
	check(InstallReadiness::shouldBlock(true, {10, 2, 0, false}),
	      "latest app with no local manifest blocks while providers are offline");
	check(!InstallReadiness::shouldBlock(true, {10, 2, 1, false}),
	      "one local latest fallback is enough to avoid a speculative block");
	check(InstallReadiness::shouldBlock(true, {10, 2, 1, true}),
	      "a pinned build stays blocked until every exact pin is local");
	check(!InstallReadiness::shouldBlock(true, {10, 2, 2, true}),
	      "a fully materialized pinned build is ready offline");

	const std::string json = InstallReadiness::serialize(123, {
		{10, 2, 0, false}, {20, 1, 1, true},
	}, true);
	check(json.find("\"version\":1") != std::string::npos
	      && json.find("\"updated_at\":123") != std::string::npos,
	      "readiness snapshot is versioned and heartbeated");
	check(json.find("\"10\":{\"blocked\":true") != std::string::npos
	      && json.find("\"20\":{\"blocked\":false") != std::string::npos,
	      "snapshot carries explicit per-app decisions");

	return failures == 0 ? 0 : 1;
}
