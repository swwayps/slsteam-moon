#pragma once

// Pure, dependency-free desktop-notification command builder.
//
// Split out of log.hpp so the notify-send mapping is unit-testable without
// dragging in openssl / config / the rest of the logger.  Owns the single
// definition of LogLevel (log.hpp includes this header for it).
//
// Note: urgency=critical notifications are "resident" in some desktop environments
// and ignore the expire timeout, staying on screen forever if no timeout is specified.
// Here every notifying level gets an explicit timeout AND a non-critical
// urgency, so they all auto-dismiss.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unistd.h>

enum class LogLevel : unsigned int
{
	//TODO: Add Trace without breaking configs and without using -1 for Once
	Once,
	Debug,
	Info,
	NotifyShort,
	NotifyLong,
	Warn,
	None
};

namespace Notify
{
	inline long long epochMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	inline long long processId()
	{
		return static_cast<long long>(::getpid());
	}

	// Escape arbitrary user-facing text into a JSON string. The Gamepad UI
	// transport uses files rather than a shell command, so it needs its own
	// encoder instead of shellEscapeDoubleQuoted().
	inline std::string jsonEscape(const std::string& value)
	{
		static constexpr char hex[] = "0123456789abcdef";
		std::string out;
		out.reserve(value.size() + 8);
		for (unsigned char c : value)
		{
			switch (c)
			{
				case '\0': break;
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20)
					{
						out += "\\u00";
						out.push_back(hex[(c >> 4) & 0x0f]);
						out.push_back(hex[c & 0x0f]);
					}
					else
					{
						out.push_back(static_cast<char>(c));
					}
			}
		}
		return out;
	}

	inline std::string buildGamepadPayload(const std::string& title,
	                                      const std::string& body,
	                                      int timeoutMs,
	                                      long long createdMs)
	{
		return "{\"version\":1,\"created_ms\":" + std::to_string(createdMs)
			+ ",\"title\":\"" + jsonEscape(title)
			+ "\",\"body\":\"" + jsonEscape(body)
			+ "\",\"timeout_ms\":" + std::to_string(timeoutMs) + "}";
	}

	// Publish one event with write-then-rename semantics. Lumen ignores .tmp
	// files, so it can never observe a partial JSON payload even when Steam is
	// interrupted during the write. This is best-effort and deliberately does
	// not depend on Decky, D-Bus, the desktop session, or a network socket.
	inline bool enqueueGamepadEvent(const std::string& queueDir,
	                                const std::string& title,
	                                const std::string& body,
	                                int timeoutMs,
	                                long long createdMs = epochMilliseconds())
	{
		static std::atomic<unsigned long long> sequence{0};
		try
		{
			const std::filesystem::path dir(queueDir);
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			if (ec) return false;
			std::filesystem::permissions(dir,
				std::filesystem::perms::owner_all,
				std::filesystem::perm_options::replace, ec);
			if (ec) return false;

			const std::string stem = "event-" + std::to_string(processId()) + "-"
				+ std::to_string(createdMs) + "-"
				+ std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
			const std::filesystem::path temporary = dir / ("." + stem + ".tmp");
			const std::filesystem::path published = dir / (stem + ".json");

			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out) return false;
				out << buildGamepadPayload(title, body, timeoutMs, createdMs);
				out.flush();
				if (!out) return false;
			}
			std::filesystem::rename(temporary, published, ec);
			if (ec)
			{
				std::filesystem::remove(temporary);
				return false;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	inline bool enqueueGamepadEventForUser(const std::string& title,
	                                       const std::string& body,
	                                       int timeoutMs)
	{
		const char* home = ::getenv("HOME");
		if (!home || !home[0]) return false;
		return enqueueGamepadEvent(std::string(home)
			+ "/.local/share/Lumen/notifications", title, body, timeoutMs);
	}

	struct Spec
	{
		bool        enabled;   // does this level raise a desktop popup?
		int         timeoutMs; // notify-send -t (auto-dismiss after this)
		const char* urgency;   // notify-send -u; never "critical" (resident)
	};

	// Maps a log level to how its desktop notification should behave.
	// Non-notifying levels return {enabled=false}.
	inline Spec specForLevel(LogLevel lvl)
	{
		switch (lvl)
		{
			case LogLevel::NotifyShort: return { true, 10000, "normal" };
			case LogLevel::NotifyLong:  return { true, 30000, "normal" };
			// Warn used to be urgency=critical with no timeout, so it never
			// auto-dismissed.  Give it the long timeout + normal urgency so
			// it behaves like the other popups.
			case LogLevel::Warn:        return { true, 30000, "normal" };
			default:                    return { false, 0, "normal" };
		}
	}

	// Decide whether a log at this level should raise a desktop popup.
	// Three independent gates, all must hold:
	//   * the level itself notifies (specForLevel(lvl).enabled);
	//   * the user has notifications turned on (config "Notifications");
	//   * the CURRENT thread is not suppressing popups.
	// The last gate exists for the background pre-warm worker: it re-stages
	// purged DLC manifests every ~30s pass, and a depot that stays gone (or
	// whose request-code keeps expiring) would otherwise fire one popup per
	// depot per pass — dozens of popups for a multi-DLC title.  The worker
	// runs on its own thread and sets the suppress flag, so its warnings
	// still reach the log file but never the screen, while the synchronous
	// install path (a different thread) keeps its user-actionable popups.
	inline bool shouldRaiseNotification(LogLevel lvl, bool notificationsEnabled,
	                                    bool threadSuppressed)
	{
		if (!notificationsEnabled) return false;
		if (threadSuppressed) return false;
		return specForLevel(lvl).enabled;
	}

	// Escape a string for safe inclusion inside a double-quoted shell word.
	// Without this a stray `"`, `$`, or backtick in a log message could break
	// the system() command or inject into the shell (fixes the old TODO in
	// log.hpp about breakage on a single `"`).
	inline std::string shellEscapeDoubleQuoted(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		for (char c : s)
		{
			// Drop embedded NULs.  __log builds its message with
			// snprintf(size)+1 then resize(size), so the std::string it
			// hands us carries a trailing '\0'.  If we copied that into the
			// command, system()'s c_str() would truncate there and lose the
			// closing quote -> shell syntax error -> no notification.
			if (c == '\0')
			{
				continue;
			}
			if (c == '\\' || c == '"' || c == '$' || c == '`')
			{
				out.push_back('\\');
			}
			out.push_back(c);
		}
		return out;
	}

	// Build the full notify-send command for a level, or "" for a
	// non-notifying level (caller then skips system()).
	inline std::string buildCommand(LogLevel lvl, const std::string& body)
	{
		const Spec s = specForLevel(lvl);
		if (!s.enabled)
		{
			return {};
		}

		std::string cmd = "notify-send -t ";
		cmd += std::to_string(s.timeoutMs);
		cmd += " -u \"";
		cmd += s.urgency;
		cmd += "\" \"SLSsteam-moon\" \"";
		cmd += shellEscapeDoubleQuoted(body);
		cmd += "\"";
		return cmd;
	}

	// Build a notify-send command from explicit parts. The user-facing popup
	// path (CLog::notifyUser) decides title/body/timeout/urgency from the
	// message catalog + severity, rather than from a LogLevel. Same shell
	// escaping as buildCommand so a stray quote / metachar in the body can't
	// break or inject into system().
	inline std::string buildCommandRaw(const std::string& title,
	                                   const std::string& body,
	                                   int timeoutMs,
	                                   const char* urgency)
	{
		std::string cmd = "notify-send -t ";
		cmd += std::to_string(timeoutMs);
		cmd += " -u \"";
		cmd += urgency;
		cmd += "\" \"";
		cmd += shellEscapeDoubleQuoted(title);
		cmd += "\" \"";
		cmd += shellEscapeDoubleQuoted(body);
		cmd += "\"";
		return cmd;
	}

	// Per-category popup throttle (anti-spam).
	//
	// Steam stages every depot of a title near-simultaneously, so a single
	// transient fault (a 503 edge, an expired request code) can fire the
	// SAME user-facing failure a dozen times within seconds — one per depot,
	// one per DLC. Without a gate that becomes a dozen identical popups.
	//
	// allow() collapses a burst of one category into a SINGLE popup per
	// cooldown window. Occurrences inside the window are counted (not shown);
	// the next allowed popup reports how many were suppressed so the user
	// still knows the scope. Categories are independent (a CDN failure does
	// not silence a config error). State is tiny and lock-free here — the
	// caller (CLog) holds its own mutex around allow().
	struct NotifyThrottle
	{
		long long cooldownMs = 60000;

		struct Entry { long long lastEmitMs; int suppressedSince; };
		std::unordered_map<int, Entry> entries;

		// Returns true if a popup for `category` should be shown now. On a
		// true return, `suppressedOut` carries how many occurrences were
		// suppressed since the previous shown popup (0 the first time).
		bool allow(int category, long long nowMs, int& suppressedOut)
		{
			auto it = entries.find(category);
			if (it == entries.end())
			{
				entries.emplace(category, Entry{ nowMs, 0 });
				suppressedOut = 0;
				return true;
			}
			Entry& e = it->second;
			if (nowMs - e.lastEmitMs >= cooldownMs)
			{
				suppressedOut = e.suppressedSince;
				e.lastEmitMs = nowMs;
				e.suppressedSince = 0;
				return true;
			}
			e.suppressedSince += 1;
			return false;
		}
	};
}
