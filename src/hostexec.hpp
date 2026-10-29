#pragma once
// The 64-bit client runs inside pressure-vessel, whose /usr is the Steam
// Runtime's: no curl, notify-send, python3 or Wine. Steam starts games through
// srt-client-container's launch-alongside-steam service, which runs a command
// in the host session of the same user. The helpers SLSsteam starts take the
// same route. Outside the container every command runs unchanged.
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace HostExec
{
	// The launch client in the Steam Runtime's /usr.
	inline constexpr const char* kLaunchClient = "/usr/bin/steam-runtime-launch-client";

	struct Route
	{
		std::string service;    // D-Bus name of the launch-alongside-steam service
		std::string busAddress; // "DBUS_SESSION_BUS_ADDRESS=..." for the launch client
	};

	inline Route& route()
	{
		static Route value;
		return value;
	}

	// A D-Bus well-known name: letters, digits, '.', '_' and '-'. Anything
	// else is refused, so the name is safe in a shell command line.
	inline bool validServiceName(const char* name)
	{
		if (name == nullptr || *name == '\0') return false;
		for (const char* c = name; *c; ++c)
		{
			const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
			             || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-';
			if (!ok) return false;
		}
		return true;
	}

	// Reads the route once, in setup(), before any helper starts. The values
	// are only read afterwards, from any thread.
	inline void init()
	{
		const char* service = std::getenv("SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM");
		const char* bus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
		if (!validServiceName(service) || bus == nullptr || *bus == '\0'
		    || access(kLaunchClient, X_OK) != 0)
			return;
		route() = {service, std::string("DBUS_SESSION_BUS_ADDRESS=") + bus};
	}

	inline bool active() { return !route().service.empty(); }

	// `command` as the argv that runs it on the host with `env` (VAR=VALUE)
	// set; unchanged when `service` is empty. SLSsteam's audit and preload
	// modules from the host session are not passed on.
	inline std::vector<std::string> argv(const std::string& service,
	                                     std::vector<std::string> command,
	                                     const std::vector<std::string>& env = {})
	{
		if (service.empty())
			return command;
		std::vector<std::string> out = {
			kLaunchClient, "--bus-name=" + service,
			"--unset-env=LD_AUDIT", "--unset-env=LD_PRELOAD",
		};
		for (const auto& variable : env)
			out.push_back("--env=" + variable);
		out.push_back("--");
		for (auto& word : command)
			out.push_back(std::move(word));
		return out;
	}

	// Shell prefix for a command line given to system(). init() only accepts
	// a service name that needs no quoting.
	inline std::string shellPrefix(const std::string& service)
	{
		if (service.empty())
			return {};
		return std::string(kLaunchClient) + " --bus-name=" + service
		     + " --unset-env=LD_AUDIT --unset-env=LD_PRELOAD -- ";
	}
}
