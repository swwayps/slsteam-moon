// HostExec: commands keep their argv outside pressure-vessel and go through
// Steam's launch-alongside-steam service inside it.
//   make test-hostexec
#include "hostexec.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void check(bool ok, const char* label)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok) ++failures;
}
}

int main()
{
	using Words = std::vector<std::string>;
	check(HostExec::argv("", {"curl", "-s", "url"}) == Words{"curl", "-s", "url"},
	      "outside the container a command is unchanged");
	check(HostExec::shellPrefix("").empty(), "outside the container a shell command is unchanged");

	const std::string name = "com.steampowered.PressureVessel.LaunchAlongsideSteam.Instance83265";
	check(HostExec::argv(name, {"bash", "run-steamless.sh", "game.exe"}, {"QUIET=1"}) == Words{
	          HostExec::kLaunchClient, "--bus-name=" + name,
	          "--unset-env=LD_AUDIT", "--unset-env=LD_PRELOAD", "--env=QUIET=1",
	          "--", "bash", "run-steamless.sh", "game.exe"},
	      "inside the container the launch client runs the command on the host");
	check(HostExec::shellPrefix(name) == std::string(HostExec::kLaunchClient) + " --bus-name=" + name
	          + " --unset-env=LD_AUDIT --unset-env=LD_PRELOAD -- ",
	      "a shell command gets the same prefix");

	check(HostExec::validServiceName(name.c_str()), "a D-Bus well-known name is accepted");
	check(!HostExec::validServiceName("a; rm -rf ~") && !HostExec::validServiceName("a$(x)")
	          && !HostExec::validServiceName("") && !HostExec::validServiceName(nullptr),
	      "a name that would need shell quoting is refused");

	unsetenv("SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM");
	HostExec::init();
	check(!HostExec::active(), "without the launcher service nothing is rerouted");
	setenv("SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM", "bad name`", 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/pressure-vessel/bus", 1);
	HostExec::init();
	check(!HostExec::active(), "an invalid service name from the environment is ignored");

	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
