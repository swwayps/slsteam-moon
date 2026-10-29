#pragma once

#include "../ascii.hpp"

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
//   - rewritePortArgInto / rewriteArgv : the argv surgery, allocation-free
//   - prepareContract / publishPrepared : the contract, published from the
//     fork child with plain system calls
//   - isBindable / pickFreePort / resolveSessionPortNoPersist : free-port selection
//   - deckyPresent / removeContract : Decky-coexistence gate (keep 8080)
// The actual exec interposition lives in main.cpp.
//
// Decky coexistence: when Decky Loader is installed we must NOT move CEF off
// 8080 (Decky's injector is hard-coded to it). main.cpp::setup() then leaves
// the port alone and Lumen falls back to 8080 too; both share the endpoint.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace CefPort
{
	inline constexpr const char* kSwitchPrefix = "--remote-debugging-port=";
	inline constexpr std::size_t kNoFit = static_cast<std::size_t>(-1);

	// The exec hook runs in a fork child right before exec, where another
	// thread of the parent may have held a lock (the logger's, the allocator's)
	// at fork time. Everything below that it calls therefore uses fixed
	// buffers and plain system calls only.

	// Copy `arg` into `out` (capacity `cap`, NUL included), replacing the digit
	// run after every "--remote-debugging-port=" occurrence with `port`.
	// Returns the length written, or kNoFit when the result does not fit.
	// Matches by prefix (not the literal 8080) so a Steam build defaulting to
	// another port is still caught, and is idempotent when the value already
	// equals `port`. Handles both a bare argv element and the switch embedded
	// in a `sh -c "exec ... '--remote-debugging-port=8080' ..."` string.
	inline std::size_t rewritePortArgInto(const char* arg, uint16_t port, char* out,
	                                      std::size_t cap, bool& changed) noexcept
	{
		char digits[6];
		std::size_t digitCount = 0;
		for (unsigned int value = port; digitCount == 0 || value != 0; value /= 10)
			digits[digitCount++] = static_cast<char>('0' + value % 10);
		const std::size_t prefixLength = std::strlen(kSwitchPrefix);
		changed = false;
		std::size_t n = 0;
		const auto put = [&](char c) {
			if (n + 1 >= cap) return false;
			out[n++] = c;
			return true;
		};
		for (std::size_t i = 0; arg[i] != '\0';)
		{
			if (std::strncmp(arg + i, kSwitchPrefix, prefixLength) != 0)
			{
				if (!put(arg[i++])) return kNoFit;
				continue;
			}
			for (std::size_t k = 0; k < prefixLength; ++k)
				if (!put(arg[i++])) return kNoFit;
			const std::size_t start = i;
			while (Ascii::isDigit(static_cast<unsigned char>(arg[i]))) ++i;
			// No digits after the prefix: not a port value, keep it as it is.
			if (i == start) continue;
			for (std::size_t k = digitCount; k > 0; --k)
				if (!put(digits[k - 1])) return kNoFit;
			changed = true;
		}
		if (cap == 0) return kNoFit;
		out[n] = '\0';
		return n;
	}

	// Static storage for one rewritten argv.
	struct ArgvArena
	{
		static constexpr std::size_t kMaxArgs = 256;
		char* argv[kMaxArgs + 1];
		char bytes[16384];
	};

	// True when `arg` carries the switch followed by a port value.
	inline bool hasPortSwitch(const char* arg) noexcept
	{
		const std::size_t prefixLength = std::strlen(kSwitchPrefix);
		for (const char* p = std::strstr(arg, kSwitchPrefix); p != nullptr;
		     p = std::strstr(p + 1, kSwitchPrefix))
		{
			if (Ascii::isDigit(static_cast<unsigned char>(p[prefixLength])))
				return true;
		}
		return false;
	}

	// The argv with the CEF debug port rewritten to `port`, built in `arena`:
	// elements without the switch are passed through, the others are copied.
	// nullptr when no element carries the switch or the result does not fit.
	inline char** rewriteArgv(char* const argv[], uint16_t port, ArgvArena& arena) noexcept
	{
		if (argv == nullptr) return nullptr;
		bool any = false;
		std::size_t used = 0;
		std::size_t n = 0;
		for (; argv[n] != nullptr; ++n)
		{
			if (n == ArgvArena::kMaxArgs) return nullptr;
			arena.argv[n] = argv[n];
			if (!hasPortSwitch(argv[n])) continue;
			bool changed = false;
			const std::size_t length = rewritePortArgInto(
				argv[n], port, arena.bytes + used, sizeof(arena.bytes) - used, changed);
			if (length == kNoFit) return nullptr;
			arena.argv[n] = arena.bytes + used;
			used += length + 1;
			any = true;
		}
		arena.argv[n] = nullptr;
		return any ? arena.argv : nullptr;
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
	// Contract format:
	//   line 1: <port>                      (all an older Lumen reads)
	//   line 2: owner <pid> <startTicks>    (optional)
	// `startTicks` is field 22 of /proc/<pid>/stat, so pid reuse cannot make a
	// stale contract look live. Lumen ignores a contract whose owner is gone.
	// Written only from the exec hook that launches the webhelper (never at
	// preinit), so a client that loses the single-instance race publishes nothing.
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

	// A contract formatted in the parent (setup()), so the exec hook can
	// publish it from the fork child with plain system calls.
	struct PreparedContract
	{
		char path[512] = {};
		char text[64] = {};
		std::size_t length = 0;
	};

	// Format the contract for `path` and create its directory. False when the
	// path is empty or too long; nothing can be published then.
	inline bool prepareContract(PreparedContract& out, const std::string& path,
	                            uint16_t port, long ownerPid,
	                            unsigned long long ownerStartTicks)
	{
		out = {};
		const std::string text = formatContract(port, ownerPid, ownerStartTicks);
		// Room for the ".tmp.<pid>" suffix publishPrepared appends.
		if (path.empty() || path.size() + 16 >= sizeof(out.path) ||
		    text.size() > sizeof(out.text))
			return false;
		std::error_code ec;
		const auto parent = std::filesystem::path(path).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent, ec); // best effort
		std::memcpy(out.path, path.c_str(), path.size() + 1);
		std::memcpy(out.text, text.data(), text.size());
		out.length = text.size();
		return true;
	}

	// Lumen polls the contract, so it is published whole: written to a
	// private temporary file, then renamed over the old one.
	inline bool publishPrepared(const PreparedContract& contract) noexcept
	{
		if (contract.length == 0) return false;
		char tmp[sizeof(contract.path)];
		std::size_t n = std::strlen(contract.path);
		std::memcpy(tmp, contract.path, n);
		std::memcpy(tmp + n, ".tmp.", 5);
		n += 5;
		char digits[12];
		std::size_t count = 0;
		for (unsigned long pid = static_cast<unsigned long>(::getpid());
		     count == 0 || pid != 0; pid /= 10)
			digits[count++] = static_cast<char>('0' + pid % 10);
		while (count > 0) tmp[n++] = digits[--count];
		tmp[n] = '\0';
		const int fd = ::open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (fd < 0) return false;
		bool ok = true;
		for (std::size_t done = 0; ok && done < contract.length;)
		{
			const ssize_t written = ::write(fd, contract.text + done, contract.length - done);
			if (written < 0 && errno == EINTR) continue;
			ok = written > 0;
			if (ok) done += static_cast<std::size_t>(written);
		}
		ok = ::close(fd) == 0 && ok;
		if (ok && ::rename(tmp, contract.path) == 0) return true;
		::unlink(tmp);
		return false;
	}

	// Decide the port to use this session WITHOUT persisting it: reuse the
	// file's port if it is a valid, currently-bindable port; otherwise pick a
	// fresh free port. Returns 0 only if no port could be obtained at all.
	//
	// The client calls this at la_preinit (setup()) to choose the
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
