#pragma once

// Pure, side-effect-light helpers for the CEF debug-port rewrite shim.
//
// Steam's client launches the CEF webhelper with a HARD-CODED
// `--remote-debugging-port=8080` (the value of the .cef-enable-remote-debugging
// flag file is ignored). That squats on TCP 8080,
// which collides with common dev servers. We can't change it by editing Steam's
// launcher scripts — Steam restores them from bootstrap on every boot — so the
// shim interposes the exec family inside the client and rewrites the argument
// in flight to a free loopback port, then publishes the chosen port to a
// contract file the Lumen sidecar reads.
//
// This header holds the parts worth unit-testing in isolation:
//   - rewritePortArg : the string surgery on one argv element
//   - isBindable / pickFreePort / resolveSessionPort : free-port selection
//   - deckyPresent / removeContract : Decky-coexistence gate (keep 8080)
// The actual exec interposition lives in main.cpp.
//
// Decky coexistence: when Decky Loader is installed we must NOT move CEF off
// 8080 (Decky's injector is hard-coded to it). main.cpp::setup() then leaves
// the port alone and Lumen falls back to 8080 too; both share the endpoint.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace CefPort
{
	inline constexpr const char* kSwitchPrefix = "--remote-debugging-port=";

	// Replace the digit run after every "--remote-debugging-port=" occurrence
	// in `arg` with `port`. Returns {rewritten, changed}. Matches by prefix
	// (not the literal 8080) so a Steam build defaulting to another port is
	// still caught, and is idempotent when the value already equals `port`.
	// Handles both a bare argv element and the switch embedded inside a
	// `sh -c "exec ... '--remote-debugging-port=8080' ..."` wrapper string.
	inline std::pair<std::string, bool> rewritePortArg(const std::string& arg, uint16_t port)
	{
		const std::string prefix = kSwitchPrefix;
		const std::string portStr = std::to_string(port);
		std::string out;
		out.reserve(arg.size() + 8);
		bool changed = false;

		size_t i = 0;
		while (i < arg.size())
		{
			if (arg.compare(i, prefix.size(), prefix) == 0)
			{
				out += prefix;
				i += prefix.size();

				size_t start = i;
				while (i < arg.size() && std::isdigit(static_cast<unsigned char>(arg[i])))
				{
					++i;
				}

				if (i > start)
				{
					out += portStr;
					changed = true;
				}
				// No digits after the prefix: leave as-is (prefix already copied).
			}
			else
			{
				out += arg[i];
				++i;
			}
		}

		return {out, changed};
	}

	// True if 127.0.0.1:port can be bound right now (plain bind, no REUSEADDR,
	// to mirror how the webhelper itself binds).
	inline bool isBindable(uint16_t port)
	{
		if (port == 0)
		{
			return false;
		}

		const int s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);

		const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
		::close(s);
		return ok;
	}

	// Ask the kernel for a free loopback port (bind :0). Returns 0 on failure.
	inline uint16_t pickFreePort()
	{
		const int s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			return 0;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;

		uint16_t port = 0;
		if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
		{
			socklen_t len = sizeof(addr);
			if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
			{
				port = ntohs(addr.sin_port);
			}
		}

		::close(s);
		return port;
	}

	inline uint16_t readPortFile(const std::string& path)
	{
		std::ifstream f(path);
		if (!f)
		{
			return 0;
		}
		long v = 0;
		f >> v;
		if (v < 1024 || v > 65535)
		{
			return 0;
		}
		return static_cast<uint16_t>(v);
	}

	// ── contract ownership ────────────────────────────────────────────────
	//
	// The contract file used to hold nothing but a port number, which made it
	// impossible for Lumen to tell "the port this session is listening on" from
	// "the port the PREVIOUS session listened on". Measured consequence: for the
	// first 4-8 s of a boot the sidecar polls the dead port of the last session
	// (it is only replaced when this client's exec hook fires), and a leftover
	// contract from an injected session makes a later VANILLA Steam launch
	// unreachable forever (Lumen never falls back to 8080).
	//
	// So the contract now carries the identity of the client that owns it:
	//   line 1: <port>                      (unchanged — old Lumen still works)
	//   line 2: owner <pid> <startTicks>    (optional; ignored by old Lumen)
	// `startTicks` is field 22 of /proc/<pid>/stat, so pid reuse cannot make a
	// stale contract look live. Lumen ignores a contract whose owner is gone.
	//
	// This is identity only: nothing here changes WHEN the contract is written.
	// It is still published exclusively from the exec hook that launches the
	// webhelper (never at preinit), which is what keeps a losing single-instance
	// client from ever publishing a port nothing listens on.
	inline std::string formatContract(uint16_t port, long ownerPid,
	                                  unsigned long long ownerStartTicks)
	{
		std::string out = std::to_string(port);
		out += "\n";
		if (ownerPid > 0 && ownerStartTicks > 0)
		{
			out += "owner ";
			out += std::to_string(ownerPid);
			out += " ";
			out += std::to_string(ownerStartTicks);
			out += "\n";
		}
		return out;
	}

	// Field 22 (starttime, in clock ticks since boot) of a /proc/<pid>/stat
	// line, or 0 when it cannot be parsed. The executable name in field 2 is
	// parenthesised and may itself contain spaces and ')', so parsing starts
	// after the LAST ')'.
	inline unsigned long long parseStatStartTicks(const std::string& stat)
	{
		const size_t close = stat.rfind(')');
		if (close == std::string::npos)
		{
			return 0;
		}

		// After the comm field the next token is field 3 (state), so field 22 is
		// the 20th token from here.
		size_t i = close + 1;
		for (int field = 3; field <= 22; ++field)
		{
			while (i < stat.size() && stat[i] == ' ')
			{
				++i;
			}
			const size_t start = i;
			while (i < stat.size() && stat[i] != ' ' && stat[i] != '\n')
			{
				++i;
			}
			if (start == i)
			{
				return 0;
			}
			if (field == 22)
			{
				try
				{
					return std::stoull(stat.substr(start, i - start));
				}
				catch (...)
				{
					return 0;
				}
			}
		}
		return 0;
	}

	// Process start time of `pid` in clock ticks, or 0 when unreadable.
	inline unsigned long long readProcStartTicks(long pid)
	{
		if (pid <= 0)
		{
			return 0;
		}
		std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
		if (!f)
		{
			return 0;
		}
		std::string line;
		std::getline(f, line);
		return parseStatStartTicks(line);
	}

	inline bool writePortFile(const std::string& path, uint16_t port, long ownerPid,
	                          unsigned long long ownerStartTicks)
	{
		std::error_code ec;
		const auto parent = std::filesystem::path(path).parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec); // best effort
		}
		std::ofstream f(path, std::ios::trunc);
		if (!f)
		{
			return false;
		}
		f << formatContract(port, ownerPid, ownerStartTicks);
		return static_cast<bool>(f);
	}

	inline bool writePortFile(const std::string& path, uint16_t port)
	{
		return writePortFile(path, port, 0, 0);
	}

	// Decide the port to use this session WITHOUT persisting it: reuse the
	// file's port if it is a valid, currently-bindable port; otherwise pick a
	// fresh free port. Returns 0 only if no port could be obtained at all.
	//
	// This is the variant the client uses at la_preinit (setup()) to choose the
	// session port early, WITHOUT touching the contract file. The file is
	// written later, only when this client tree actually launches the webhelper
	// (see main.cpp). That matters at login autostart, where two Steam instances
	// can start concurrently: both run setup(), but only the one that wins the
	// single-instance race goes on to spawn a webhelper. If setup() itself wrote
	// the contract, the LOSING instance (which picks a different free port, then
	// exits before spawning anything) would clobber it with a port nothing ends
	// up listening on, and the Lumen sidecar would connect to a dead port.
	inline uint16_t resolveSessionPortNoPersist(const std::string& path)
	{
		const uint16_t existing = readPortFile(path);
		if (existing != 0 && isBindable(existing))
		{
			return existing;
		}
		return pickFreePort();
	}

	// Resolve the port to use this session: reuse the file's port if it is a
	// valid, currently-bindable port; otherwise pick a fresh free port and
	// persist it. Returns 0 only if no port could be obtained at all (caller
	// then leaves the argv untouched -> Steam keeps 8080).
	inline uint16_t resolveSessionPort(const std::string& path)
	{
		const uint16_t existing = readPortFile(path);
		if (existing != 0 && isBindable(existing))
		{
			return existing;
		}

		const uint16_t fresh = pickFreePort();
		if (fresh != 0)
		{
			writePortFile(path, fresh);
		}
		return fresh;
	}

	// Contract file shared with the Lumen sidecar:
	// $HOME/.local/share/Lumen/cef_port  ("" if HOME is unset).
	inline std::string contractPath()
	{
		const char* home = std::getenv("HOME");
		if (!home || !*home)
		{
			return "";
		}
		return std::string(home) + "/.local/share/Lumen/cef_port";
	}

	// True when Decky Loader is installed for this user. Decky's injector is
	// HARD-CODED to http://localhost:8080 (decky_loader/injector.py
	// BASE_ADDRESS) and runs as a persistent root daemon polling 8080 in BOTH
	// Desktop and Game Mode, so it cannot follow our ephemeral port. When Decky
	// is present we must NOT move Steam's CEF endpoint off 8080: instead we
	// leave it on the default and let Lumen fall back to 8080 too (verified on a
	// Bazzite box: multiple CDP clients coexist on one CEF target, so Decky and
	// Lumen share 8080 without either kicking the other).
	//
	// Marker: the canonical Decky install drops its loader binary at
	// $HOME/homebrew/services/PluginLoader (user-readable, no root needed; the
	// decky-installer uninstall removes it). `home` is taken as a parameter so
	// the check is unit-testable against a synthetic fixture.
	inline bool deckyPresent(const std::string& home)
	{
		if (home.empty())
		{
			return false;
		}
		std::error_code ec;
		return std::filesystem::exists(home + "/homebrew/services/PluginLoader", ec);
	}

	inline bool deckyPresent()
	{
		const char* home = std::getenv("HOME");
		if (!home || !*home)
		{
			return false;
		}
		return deckyPresent(std::string(home));
	}

	// Best-effort removal of the Lumen contract file. Called when we decide to
	// keep CEF on 8080 (Decky coexistence): a STALE contract left by a previous
	// ephemeral-port session would otherwise make Lumen connect to a dead port
	// instead of falling back to 8080. Idempotent; safe under concurrent
	// instances (both just unlink the same path).
	inline void removeContract(const std::string& path)
	{
		if (path.empty())
		{
			return;
		}
		std::error_code ec;
		std::filesystem::remove(path, ec); // best effort
	}
}
