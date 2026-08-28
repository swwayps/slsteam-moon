#pragma once

#include <cstddef>
#include <cstring>

namespace AuditBinding
{
	enum class Symbol
	{
		None,
		Execv,
		Execvp,
		Execve,
		Execvpe,
		PosixSpawn,
		PosixSpawnp,
		LocalStatsEpoch,
	};

	// Keep the hot-path classifier deliberately small: only names beginning with
	// the relevant initials pay for strlen/strcmp. Exact matches prevent a
	// near-miss from entering the wrapper dispatch.
	inline Symbol classify(const char* name) noexcept
	{
		if (name == nullptr)
			return Symbol::None;

		switch (name[0])
		{
			case 's':
				return std::strcmp(name, "slsteam_local_stats_epoch_v1") == 0
					? Symbol::LocalStatsEpoch : Symbol::None;
			case 'e':
			{
				const std::size_t length = std::strlen(name);
				switch (length)
				{
					case 5:
						return std::strcmp(name, "execv") == 0
							? Symbol::Execv : Symbol::None;
					case 6:
						if (std::strcmp(name, "execvp") == 0)
							return Symbol::Execvp;
						if (std::strcmp(name, "execve") == 0)
							return Symbol::Execve;
						return Symbol::None;
					case 7:
						return std::strcmp(name, "execvpe") == 0
							? Symbol::Execvpe : Symbol::None;
					default:
						return Symbol::None;
				}
			}
			case 'p':
			{
				const std::size_t length = std::strlen(name);
				if (length == 11 && std::strcmp(name, "posix_spawn") == 0)
					return Symbol::PosixSpawn;
				if (length == 12 && std::strcmp(name, "posix_spawnp") == 0)
					return Symbol::PosixSpawnp;
				return Symbol::None;
			}
			default:
				return Symbol::None;
		}
	}
}
