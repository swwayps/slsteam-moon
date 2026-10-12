#pragma once

#include "notify.hpp" // LogLevel + the notify-send command builder
#include "usermsg.hpp" // user-facing message catalog (UserMsg/Lang/messageFor)

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <openssl/sha.h>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

// Upstream's level flags, accepted by LOG_CUSTOM below. The local logger keeps
// its own threshold (LogLevel); these only select which local level a line
// goes to.
typedef uint32_t LogLevelFlags_t;

enum ELogLevel : LogLevelFlags_t
{
	k_ELogLevelTrace = 1 << 0,
	k_ELogLevelOnce = 1 << 1,
	k_ELogLevelDebug = 1 << 2,
	k_ELogLevelWarn = 1 << 3,
	k_ELogLevelError = 1 << 4,
	k_ELogLevelInfo = 1 << 5,
	k_ELogLevelNotifyShort = 1 << 6,
	k_ELogLevelNotifyLong = 1 << 7,
	k_ELogLevelAPI = 1 << 8
};

// Per-thread popup suppression: the thread's log output still reaches the
// file but never raises a desktop popup.
inline thread_local bool t_suppressNotify = false;

// RAII helper to suppress popups for the current scope (restores the prior
// value on exit), for callers that want scoped rather than whole-thread
// suppression.
struct ScopedNotifySuppression
{
	bool m_prev;
	ScopedNotifySuppression() : m_prev(t_suppressNotify) { t_suppressNotify = true; }
	~ScopedNotifySuppression() { t_suppressNotify = m_prev; }
};

class CLog
{
	std::ofstream ofstream;
	std::unordered_set<std::string> msgHist {};
	std::shared_mutex mutex;

	// User-facing popup state. m_lang is resolved once from the environment
	// at construction; m_notifyThrottle collapses per-category bursts into a
	// single popup (see Notify::NotifyThrottle). Guarded by m_notifyMu, which
	// is independent of `mutex` so a popup never contends with file writes.
	Lang m_lang = Lang::English;
	Notify::NotifyThrottle m_notifyThrottle {};
	std::mutex m_notifyMu;

	constexpr const char* logLvlToStr(LogLevel& lvl)
	{
		switch(lvl)
		{
			case LogLevel::Once:
				return "Once";
			case LogLevel::Debug:
				return "Debug";
			case LogLevel::Info:
				return "Info";
			case LogLevel::NotifyShort:
			case LogLevel::NotifyLong:
				return "Notify";
			case LogLevel::Warn:
				return "Warn";

			//Shut gcc warning up
			default:
				return "Unknown";
		}
	}

	// Writes one formatted message. A LogLevel::Once message is written once
	// per session.
	__attribute__((hot))
	void write(LogLevel lvl, const std::string& formatted)
	{
		// NOTE: writing never raises desktop popups. Every log level here
		// (warn/notify/info/...) writes ONLY to the file, so the raw
		// developer diagnostics stay available for debugging without ever
		// reaching the user's screen. The single popup path is notifyUser(),
		// which renders a friendly, localized, throttled message from the
		// usermsg.hpp catalog instead.

		const auto lock = std::lock_guard(mutex);

		//Prevent crashes from queued operations
		if (!ofstream.is_open())
		{
			return;
		}

		if (lvl == LogLevel::Once)
		{
			for(const auto& oldMsg : msgHist)
			{
				if (oldMsg == formatted)
				{
					return;
				}
			}

			msgHist.emplace(formatted);
		}

		ofstream << "[" << logLvlToStr(lvl) << "] " << formatted.c_str();
		if (lvl == LogLevel::NotifyShort || lvl == LogLevel::NotifyLong)
		{
			ofstream << "\n";
		}

		ofstream.flush();
	}

	__attribute__((format(printf, 1, 0)))
	static std::string vformat(const char* msg, va_list args)
	{
		va_list sizing;
		va_copy(sizing, args);
		const int size = vsnprintf(nullptr, 0, msg, sizing);
		va_end(sizing);
		if (size <= 0) return {};
		std::string formatted(static_cast<size_t>(size) + 1, '\0');
		vsnprintf(formatted.data(), formatted.size(), msg, args);
		formatted.resize(static_cast<size_t>(size));
		return formatted;
	}

	// Per-(level,message) deduplication state used by the *Once
	// helpers below.  We can't piggy-back on msgHist because that
	// set holds raw strings and is also touched by LogLevel::Once
	// — we want suppression scoped to the helper, not to literal
	// equality across levels.
	std::unordered_set<std::string> dedupHist {};

	bool seenBefore(LogLevel lvl, const std::string& formatted)
	{
		std::unique_lock lock(mutex);
		const std::string key =
			std::string(logLvlToStr(lvl)) + "|" + formatted;
		auto [_, inserted] = dedupHist.emplace(key);
		return !inserted;
	}

public:
	std::string path;

	CLog(const char* path);
	~CLog();

	// Every message is a printf format that the compiler checks against its
	// arguments (the build makes a mismatch an error). Pass run-time text as
	// ("%s", text).
	__attribute__((format(printf, 1, 2)))
	static std::string format(const char* msg, ...)
	{
		va_list args;
		va_start(args, msg);
		std::string formatted = vformat(msg, args);
		va_end(args);
		return formatted;
	}

	// Formats and writes one message at `lvl`; `dedupe` drops a message that
	// this level already wrote (the *Once helpers).
	__attribute__((format(printf, 4, 0)))
	void vwrite(LogLevel lvl, bool dedupe, const char* msg, va_list args)
	{
		if (lvl < getMinLevel())
		{
			return;
		}
		const std::string formatted = vformat(msg, args);
		if (dedupe && seenBefore(lvl, formatted))
		{
			return;
		}
		write(lvl, formatted);
	}

	__attribute__((format(printf, 2, 3))) void once(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Once, false, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void debug(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Debug, false, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void info(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Info, false, msg, a); va_end(a); }

	// Variants of debug/info that suppress repeated messages with
	// identical formatted text.  Use these for diagnostics that fire
	// per-app/per-depot/etc. — the first occurrence is logged, the
	// rest are squelched, so a hot loop doesn't drown the file.
	__attribute__((format(printf, 2, 3))) void infoOnce(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Info, true, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void debugOnce(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Debug, true, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void notify(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::NotifyShort, false, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void notifyLong(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::NotifyLong, false, msg, a); va_end(a); }

	__attribute__((format(printf, 2, 3))) void warn(const char* msg, ...)
	{ va_list a; va_start(a, msg); vwrite(LogLevel::Warn, false, msg, a); va_end(a); }

	// Upstream's LOG_CUSTOM: the most severe level in `flags` picks the local
	// level, k_ELogLevelOnce deduplicates info and debug lines.
	__attribute__((format(printf, 3, 4)))
	void custom(uint32_t flags, const char* msg, ...)
	{
		const bool onceFlag = flags & k_ELogLevelOnce;
		LogLevel lvl = LogLevel::Once;
		bool dedupe = false;
		if (flags & (k_ELogLevelWarn | k_ELogLevelError))
			lvl = LogLevel::Warn;
		else if (flags & k_ELogLevelNotifyLong)
			lvl = LogLevel::NotifyLong;
		else if (flags & k_ELogLevelNotifyShort)
			lvl = LogLevel::NotifyShort;
		else if (flags & (k_ELogLevelInfo | k_ELogLevelAPI))
		{
			lvl = LogLevel::Info;
			dedupe = onceFlag;
		}
		else if (flags & k_ELogLevelDebug)
		{
			lvl = LogLevel::Debug;
			dedupe = onceFlag;
		}
		else if (!onceFlag)
		{
			return;
		}
		va_list a;
		va_start(a, msg);
		vwrite(lvl, dedupe, msg, a);
		va_end(a);
	}

	// The ONLY desktop-popup path. Renders a friendly, localized message
	// from the usermsg.hpp catalog, throttled per-category so a multi-depot
	// burst collapses into a single popup. `detail` fills the catalog's
	// "{detail}" slot when present (e.g. "HTTP 503") and is omitted
	// otherwise. The raw developer diagnostic should still be logged
	// separately (warn/info) at the call site for debugging.
	void notifyUser(UserMsg msg, const std::string& detail = "");

	//Do not include config.hpp in this header, otherwise things will break :) (proly due to recursive inclusion)
	static LogLevel getMinLevel();
	static bool shouldNotify();
	static CLog* createDefaultLog();
};

extern std::unique_ptr<CLog> g_pLog;

// Upstream's logging macros on the local logger, so merged upstream code
// compiles unchanged. Trace lines compile out as in upstream's default
// builds; there is no separate error level, so errors log as warnings.
#define LOG_TRACE(fmt, ...) ((void)0)
#define LOG_TRACE_ONCE(fmt, ...) ((void)0)
#define LOG_ONCE(fmt, ...) g_pLog->once(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) g_pLog->debug(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG_ONCE(fmt, ...) g_pLog->debugOnce(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(fmt, ...) g_pLog->info(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARN(fmt, ...) g_pLog->warn(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(fmt, ...) g_pLog->warn(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_NOTIFY(fmt, ...) g_pLog->notify(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_NOTIFYLONG(fmt, ...) g_pLog->notifyLong(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_NOTIFYWARN(fmt, ...) g_pLog->warn(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_NOTIFYERROR(fmt, ...) g_pLog->warn(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_API(fmt, ...) g_pLog->info(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOG_CUSTOM(flags, fmt, ...) g_pLog->custom(flags, fmt __VA_OPT__(,) __VA_ARGS__)
