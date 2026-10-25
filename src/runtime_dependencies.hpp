#pragma once

#include <string>

namespace RuntimeDependencies
{
	// Build one stable detail string for the user-facing warning.  Kept pure
	// so the diagnostic remains unit-testable without Steam or the logger.
	inline std::string missingTools(bool hasTimeout, bool hasPython3)
	{
		std::string missing;
		if (!hasTimeout) missing = "timeout";
		if (!hasPython3)
		{
			if (!missing.empty()) missing += ", ";
			missing += "python3";
		}
		return missing;
	}

	// Format missing helper names from an explicit PATH value.  This
	// deterministic PATH probe is used by check() and is exposed so tests can
	// supply a fixture without depending on the host's PATH or logger.
	std::string missingToolsOnPath(const char* path);

	// Probe the programs tools/steamstub-bypass/run-steamless.sh needs, once
	// per process. Returns true when all are available.
	bool check();

}
