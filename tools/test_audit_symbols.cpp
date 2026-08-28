// TDD regression test for the audit binding symbol classifier.
// The classifier must reject near-misses before the callback dispatches any
// wrapper, while preserving the six exec/spawn symbols used by Steam's CEF
// launch path.

#include "audit_symbols.hpp"

#include <cstdio>
#include <string>
#include <utility>
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
	using AuditBinding::Symbol;
	const std::vector<std::pair<const char*, Symbol>> supported =
	{
		{"execv", Symbol::Execv},
		{"execvp", Symbol::Execvp},
		{"execve", Symbol::Execve},
		{"execvpe", Symbol::Execvpe},
		{"posix_spawn", Symbol::PosixSpawn},
		{"posix_spawnp", Symbol::PosixSpawnp},
		{"slsteam_local_stats_epoch_v1", Symbol::LocalStatsEpoch},
	};

	for (const auto& [name, expected] : supported)
	{
		check(AuditBinding::classify(name) == expected, name);
	}

	const std::vector<const char*> rejected =
	{
		nullptr,
		"",
		"exec",
		"execvx",
		"execV",
		"posix_spawnx",
		"spawn",
		"eexecv",
		"slsteam_local_stats_epoch_v2",
		"slsteam_local_stats_epoch_v1_extra",
	};
	for (const char* name : rejected)
	{
		check(AuditBinding::classify(name) == Symbol::None,
		      name == nullptr ? "null symbol" : name);
	}

	if (failures != 0)
	{
		std::fprintf(stderr, "test_audit_symbols: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("audit symbol tests passed");
	return 0;
}
