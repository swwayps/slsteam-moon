// SPDX-License-Identifier: AGPL-3.0-only

#include "installreadiness.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace InstallReadiness
{
	bool publish(const std::vector<Observation>& observations,
	             bool providersOffline)
	{
		const char* home = std::getenv("HOME");
		if (!home || !*home) return false;
		const std::filesystem::path dir =
		    std::filesystem::path(home) / ".config" / "SLSsteam";
		const std::filesystem::path target = dir / "install-readiness.json";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) return false;
		const std::filesystem::path temporary = target.string() + ".tmp."
		    + std::to_string(static_cast<unsigned long>(getpid()));
		const std::string body = serialize(
		    static_cast<std::int64_t>(std::time(nullptr)), observations,
		    providersOffline);
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output) return false;
			output.write(body.data(), static_cast<std::streamsize>(body.size()));
			output.close();
			if (!output) { std::filesystem::remove(temporary, ec); return false; }
		}
		(void)chmod(temporary.c_str(), 0600);
		std::filesystem::rename(temporary, target, ec);
		if (ec) { std::filesystem::remove(temporary, ec); return false; }
		return true;
	}
}
